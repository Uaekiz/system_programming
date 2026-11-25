#include "process_manager.h"
#include "logger.h"
#include "signal_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

child_process_t process_table[MAX_CHILDREN];
int active_child_count = 0;

static void recalculate_active_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (process_table[i].state == PROC_RUNNING) {
            count++;
        }
    }
    active_child_count = count;
}

static void reset_child_signals(void) {
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    sigprocmask(SIG_SETMASK, &empty_mask, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}

void init_process_manager(void) {
    memset(process_table, 0, sizeof(process_table));
    for (int i = 0; i < MAX_CHILDREN; i++) {
        process_table[i].id = i + 1;
        process_table[i].pid = -1;
        process_table[i].state = PROC_UNUSED;
        process_table[i].restart_count = 0;
        process_table[i].last_exit_status = 0;
        process_table[i].start_time = 0;
        snprintf(process_table[i].name, sizeof(process_table[i].name), "worker_%d", i + 1);
        snprintf(process_table[i].arg, sizeof(process_table[i].arg), "normal");
    }
    active_child_count = 0;
    log_event("INFO", "Process manager initialized (capacity: %d children, max restarts: %d).",
              MAX_CHILDREN, MAX_RESTARTS);
}

pid_t start_child_process(int index, const char *name, const char *mode_arg) {
    if (shutdown_flag) {
        log_event("INFO", "Shutdown in progress; skipping start of child at index %d.", index);
        return -1;
    }

    if (index < 0 || index >= MAX_CHILDREN) {
        log_event("ERROR", "Invalid process table index: %d", index);
        return -1;
    }

    child_process_t *proc = &process_table[index];

    if (name != NULL && name[0] != '\0') {
        snprintf(proc->name, sizeof(proc->name), "%s", name);
    }
    if (mode_arg != NULL && mode_arg[0] != '\0') {
        snprintf(proc->arg, sizeof(proc->arg), "%s", mode_arg);
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_event("ERROR", "fork() failed for %s: %s", proc->name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Put the child into its own process group (terminal Ctrl+C only reaches the watchdog)
        setpgid(0, 0);
        reset_child_signals();

        char *args[] = {(char *)WORKER_PATH, proc->arg, NULL};
        execv(WORKER_PATH, args);

        perror("execv failed");
        _exit(EXIT_FAILURE);
    }

    proc->pid = pid;
    proc->state = PROC_RUNNING;
    proc->start_time = time(NULL);
    recalculate_active_count();

    log_event("INFO", "Started child [%s] (ID=%d, PID=%d, mode=%s, restarts=%d/%d).",
              proc->name, proc->id, (int)pid, proc->arg, proc->restart_count, MAX_RESTARTS);

    return pid;
}

void start_initial_children(int count) {
    if (count < 2) {
        count = 2;
    }
    if (count > MAX_CHILDREN) {
        count = MAX_CHILDREN;
    }

    for (int i = 0; i < count; i++) {
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "worker_%d", i + 1);
        start_child_process(i, name_buf, "normal");
    }
}

void reap_terminated_children(void) {
    int status;
    pid_t pid;

    while (1) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pid == 0) {
            break;
        }

        int found_idx = -1;
        for (int i = 0; i < MAX_CHILDREN; i++) {
            if (process_table[i].pid == pid && process_table[i].state == PROC_RUNNING) {
                found_idx = i;
                break;
            }
        }

        if (found_idx == -1) {
            for (int i = 0; i < MAX_CHILDREN; i++) {
                if (process_table[i].pid == pid) {
                    found_idx = i;
                    break;
                }
            }
        }

        if (found_idx == -1) {
            log_event("WARN", "Reaped untracked child PID=%d.", (int)pid);
            continue;
        }

        child_process_t *proc = &process_table[found_idx];

        if (proc->state == PROC_STOPPED) {
            proc->last_exit_status = WIFEXITED(status) ? WEXITSTATUS(status)
                                                       : (WIFSIGNALED(status) ? (128 + WTERMSIG(status)) : 0);
            log_event("INFO", "Stopped child [%s] (PID=%d) reaped cleanly.", proc->name, (int)pid);
            continue;
        }

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            proc->last_exit_status = exit_code;

            if (exit_code == 0) {
                proc->state = PROC_EXITED_NORMAL;
                log_event("INFO", "Child [%s] (PID=%d) exited normally (status=0). No restart needed.",
                          proc->name, (int)pid);
            } else {
                proc->state = PROC_CRASHED;
                log_event("WARN", "Child [%s] (PID=%d) terminated unexpectedly with exit code %d.",
                          proc->name, (int)pid, exit_code);
            }
        } else if (WIFSIGNALED(status)) {
            int term_sig = WTERMSIG(status);
            proc->last_exit_status = 128 + term_sig;
            proc->state = PROC_CRASHED;
            log_event("WARN", "Child [%s] (PID=%d) killed by signal %d.",
                      proc->name, (int)pid, term_sig);
        } else {
            proc->state = PROC_CRASHED;
            log_event("WARN", "Child [%s] (PID=%d) terminated with unknown status.",
                      proc->name, (int)pid);
        }
    }

    recalculate_active_count();
}

void check_and_restart_children(void) {
    if (shutdown_flag) {
        return;
    }

    for (int i = 0; i < MAX_CHILDREN; i++) {
        child_process_t *proc = &process_table[i];

        if (proc->state != PROC_CRASHED) {
            continue;
        }

        if (proc->restart_count < MAX_RESTARTS) {
            proc->restart_count++;
            log_event("INFO", "Restarting child [%s] (attempt %d/%d)...",
                      proc->name, proc->restart_count, MAX_RESTARTS);

            if (start_child_process(i, NULL, NULL) < 0) {
                proc->state = PROC_FAILED_LIMIT;
                log_event("ERROR", "Failed to restart child [%s]; marking as FAILED_LIMIT.",
                          proc->name);
            }
        } else {
            proc->state = PROC_FAILED_LIMIT;
            log_event("ERROR", "Child [%s] exceeded max restart limit (%d/%d). Giving up.",
                      proc->name, proc->restart_count, MAX_RESTARTS);
        }
    }

    recalculate_active_count();
}

void setup_periodic_timer(int seconds) {
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));

    if (seconds > 0) {
        timer.it_value.tv_sec = seconds;
        timer.it_interval.tv_sec = seconds;
    }

    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        log_event("ERROR", "setitimer(ITIMER_REAL) failed: %s", strerror(errno));
    } else if (seconds > 0) {
        log_event("INFO", "Periodic health check timer configured (%d second interval).", seconds);
    }
}

void perform_periodic_health_check(void) {
    reap_terminated_children();

    for (int i = 0; i < MAX_CHILDREN; i++) {
        child_process_t *proc = &process_table[i];
        if (proc->state == PROC_RUNNING && proc->pid > 0) {
            if (kill(proc->pid, 0) == -1 && errno == ESRCH) {
                log_event("WARN", "Health check detected missing process [%s] (PID=%d).",
                          proc->name, (int)proc->pid);
                proc->state = PROC_CRASHED;
            }
        }
    }

    check_and_restart_children();
    recalculate_active_count();
    log_event("INFO", "Periodic health check completed (active children: %d).", active_child_count);
}