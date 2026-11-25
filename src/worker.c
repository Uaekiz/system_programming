#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t terminate_requested = 0;

static void handle_sigterm(int sig) {
    (void)sig;
    terminate_requested = 1;
}

static void setup_worker_signals(int ignore_sigterm) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (ignore_sigterm) {
        sa.sa_handler = SIG_IGN;
    } else {
        sa.sa_handler = handle_sigterm;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("worker: sigaction(SIGTERM) failed");
        exit(EXIT_FAILURE);
    }
}

static void sleep_interruptible(unsigned int seconds) {
    unsigned int remaining = seconds;
    while (remaining > 0 && !terminate_requested) {
        remaining = sleep(remaining);
    }
}

int main(int argc, char *argv[]) {
    const char *mode = "normal";
    if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
        mode = argv[1];
    }

    pid_t pid = getpid();
    int ignore_sigterm = (strcmp(mode, "hang") == 0);
    setup_worker_signals(ignore_sigterm);

    printf("[Worker PID=%d] Started in '%s' mode.\n", (int)pid, mode);
    fflush(stdout);

    if (strcmp(mode, "short") == 0) {
        // Normal termination test: runs for 3 seconds and exits via exit(0)
        sleep_interruptible(3);
        printf("[Worker PID=%d] Completed work normally (exit 0).\n", (int)pid);
        fflush(stdout);
        return EXIT_SUCCESS;
    } else if (strcmp(mode, "crash") == 0) {
        // Unexpected termination test: exits with an error code after 2 seconds
        sleep_interruptible(2);
        if (!terminate_requested) {
            fprintf(stderr, "[Worker PID=%d] Simulating unexpected error exit (exit 1)!\n", (int)pid);
            fflush(stderr);
            return EXIT_FAILURE;
        }
    } else if (strcmp(mode, "hang") == 0) {
        // Timeout (SIGKILL) test: ignores the SIGTERM signal
        while (1) {
            sleep(5);
        }
    } else {
        // "normal" (default) mode: keeps running until SIGTERM arrives
        while (!terminate_requested) {
            sleep(1);
        }
    }

    printf("[Worker PID=%d] Received SIGTERM, exiting gracefully.\n", (int)pid);
    fflush(stdout);
    return EXIT_SUCCESS;
}