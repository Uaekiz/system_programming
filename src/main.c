#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_INITIAL_CHILDREN 2
#define DEFAULT_HEALTH_CHECK_INTERVAL 5

static int is_valid_worker_mode(const char *arg) {
    return (strcmp(arg, "normal") == 0 ||
            strcmp(arg, "short") == 0 ||
            strcmp(arg, "crash") == 0 ||
            strcmp(arg, "hang") == 0);
}

static void launch_workers_from_args(int argc, char *argv[]) {
    if (argc <= 1) {
        start_initial_children(DEFAULT_INITIAL_CHILDREN);
        return;
    }

    // Sayısal argüman verildiyse (örn: ./watchdog 3) o sayıda normal worker başlat
    if (argc == 2 && !is_valid_worker_mode(argv[1])) {
        char *endptr = NULL;
        long count = strtol(argv[1], &endptr, 10);
        if (endptr != argv[1] && *endptr == '\0' && count > 0) {
            start_initial_children((int)count);
            return;
        }
    }

    // Worker çalışma modları argüman olarak verildiyse (örn: ./watchdog short normal)
    int launched = 0;
    for (int i = 1; i < argc && launched < MAX_CHILDREN; i++) {
        const char *mode = is_valid_worker_mode(argv[i]) ? argv[i] : "normal";
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "worker_%d", launched + 1);
        if (start_child_process(launched, name_buf, mode) > 0) {
            launched++;
        }
    }

    // En az 2 alt süreç kuralını korumak için eksik kalırsa normal worker ile tamamla
    while (launched < DEFAULT_INITIAL_CHILDREN && launched < MAX_CHILDREN) {
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "worker_%d", launched + 1);
        if (start_child_process(launched, name_buf, "normal") > 0) {
            launched++;
        } else {
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    // 1. Önce loglayıcıyı başlat
    if (init_logger(DEFAULT_LOG_FILE) != 0) {
        fprintf(stderr, "Fatal: Failed to initialize logger.\n");
        return EXIT_FAILURE;
    }

    log_event("INFO", "Watchdog supervisor starting (PID=%d)...", (int)getpid());

    // 2. Süreç yöneticisi tablosunu hazırla
    init_process_manager();

    // 3. Alt süreçler başlatılmadan önce sinyal yakalayıcıları kur (SIGCHLD kaybını önler)
    if (setup_signal_handlers() != 0) {
        log_event("ERROR", "Fatal: Failed to install signal handlers.");
        close_logger();
        return EXIT_FAILURE;
    }

    // 4. Başlangıç alt süreçlerini ve periyodik sağlık kontrolü zamanlayıcısını başlat
    launch_workers_from_args(argc, argv);
    setup_periodic_timer(DEFAULT_HEALTH_CHECK_INTERVAL);

    // Sinyal bekleme sırasında yarış durumunu (race condition) önlemek için sinyal maskesi
    sigset_t wait_mask;
    sigset_t orig_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGINT);
    sigaddset(&wait_mask, SIGTERM);
    sigaddset(&wait_mask, SIGUSR1);
    sigaddset(&wait_mask, SIGCHLD);
    sigaddset(&wait_mask, SIGALRM);

    // 5. Ana kontrol döngüsü
    while (!shutdown_flag) {
        if (sigchld_flag) {
            sigchld_flag = 0;
            reap_terminated_children();
            check_and_restart_children();
        }

        if (sigusr1_flag) {
            sigusr1_flag = 0;
            log_process_status_report();
        }

        if (sigalrm_flag) {
            sigalrm_flag = 0;
            perform_periodic_health_check();
        }

        if (shutdown_flag) {
            break;
        }

        // Sinyalleri atomik olarak beklerken CPU tüketimini %0'da tut
        sigprocmask(SIG_BLOCK, &wait_mask, &orig_mask);
        if (!shutdown_flag && !sigchld_flag && !sigusr1_flag && !sigalrm_flag) {
            sigsuspend(&orig_mask);
        }
        sigprocmask(SIG_SETMASK, &orig_mask, NULL);
    }

    // 6. Kontrollü kapanış ve kaynak temizliği
    const char *sig_name = (shutdown_signal == SIGINT)    ? "SIGINT"
                           : (shutdown_signal == SIGTERM) ? "SIGTERM"
                                                          : "SIGNAL";
    log_event("INFO", "Shutdown triggered by %s. Initiating graceful shutdown...", sig_name);

    graceful_shutdown(DEFAULT_SHUTDOWN_TIMEOUT);
    close_logger();

    return EXIT_SUCCESS;
}