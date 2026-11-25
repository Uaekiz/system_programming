#include "signal_manager.h"
#include "logger.h"
#include "process_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t sigchld_flag = 0;
volatile sig_atomic_t sigusr1_flag = 0;
volatile sig_atomic_t sigalrm_flag = 0;
volatile sig_atomic_t shutdown_flag = 0;
volatile sig_atomic_t shutdown_signal = 0;

// Async-signal-safe handler functions (only the volatile sig_atomic_t flags are updated)
static void handle_sigint(int sig) {
    (void)sig;
    shutdown_signal = SIGINT;
    shutdown_flag = 1;
}

static void handle_sigterm(int sig) {
    (void)sig;
    shutdown_signal = SIGTERM;
    shutdown_flag = 1;
}

static void handle_sigusr1(int sig) {
    (void)sig;
    sigusr1_flag = 1;
}

static void handle_sigchld(int sig) {
    (void)sig;
    sigchld_flag = 1;
}

static void handle_sigalrm(int sig) {
    (void)sig;
    sigalrm_flag = 1;
}

static int install_handler(int signum, void (*handler)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);

    // Prevent other managed signals from interrupting while the handler is running
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaddset(&sa.sa_mask, SIGCHLD);
    sigaddset(&sa.sa_mask, SIGALRM);

    // SA_RESTART is not used: blocking calls in the main loop must wake up immediately when a signal arrives
    sa.sa_flags = flags;

    if (sigaction(signum, &sa, NULL) == -1) {
        perror("setup_signal_handlers: sigaction failed");
        return -1;
    }

    return 0;
}

int setup_signal_handlers(void) {
    if (install_handler(SIGINT, handle_sigint, 0) == -1) {
        return -1;
    }
    if (install_handler(SIGTERM, handle_sigterm, 0) == -1) {
        return -1;
    }
    if (install_handler(SIGUSR1, handle_sigusr1, 0) == -1) {
        return -1;
    }
    // SA_NOCLDSTOP: generates SIGCHLD only when a child terminates (not when it is stopped)
    if (install_handler(SIGCHLD, handle_sigchld, SA_NOCLDSTOP) == -1) {
        return -1;
    }
    if (install_handler(SIGALRM, handle_sigalrm, 0) == -1) {
        return -1;
    }

    return 0;
}

static int find_child_index_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (process_table[i].state == PROC_RUNNING && process_table[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int count_running_children(void) {
    int count = 0;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (process_table[i].state == PROC_RUNNING && process_table[i].pid > 0) {
            count++;
        }
    }
    return count;
}

static double get_elapsed_seconds(const struct timespec *start, const struct timespec *now) {
    double sec = (double)(now->tv_sec - start->tv_sec);
    double nsec = (double)(now->tv_nsec - start->tv_nsec) / 1e9;
    return sec + nsec;
}

void graceful_shutdown(int timeout_sec) {
    if (timeout_sec < 0) {
        timeout_sec = 0;
    }

    // Stop the periodic alarm from firing again during shutdown
    struct itimerval disable_timer;
    memset(&disable_timer, 0, sizeof(disable_timer));
    setitimer(ITIMER_REAL, &disable_timer, NULL);

    log_event("INFO", "Graceful shutdown initiated (timeout: %d seconds).", timeout_sec);

    // Phase 1: Send SIGTERM to all running child processes
    int signaled_count = 0;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (process_table[i].state == PROC_RUNNING && process_table[i].pid > 0) {
            if (kill(process_table[i].pid, SIGTERM) == 0) {
                signaled_count++;
                log_event("INFO", "Sent SIGTERM to %s (PID=%d).",
                          process_table[i].name, (int)process_table[i].pid);
            } else if (errno != ESRCH) {
                log_event("ERROR", "Failed to send SIGTERM to %s (PID=%d): %s",
                          process_table[i].name, (int)process_table[i].pid, strerror(errno));
            }
        }
    }

    if (signaled_count == 0 && count_running_children() == 0) {
        active_child_count = 0;
        log_event("INFO", "No active child processes to terminate. Shutdown complete.");
        return;
    }

    // Phase 2: Reap terminated children with waitpid(WNOHANG) throughout the timeout period
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (count_running_children() > 0) {
        int status = 0;
        pid_t reaped_pid;

        while ((reaped_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            int idx = find_child_index_by_pid(reaped_pid);
            if (idx >= 0) {
                process_table[idx].state = PROC_STOPPED;
                if (WIFEXITED(status)) {
                    process_table[idx].last_exit_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    process_table[idx].last_exit_status = WTERMSIG(status);
                }
                if (active_child_count > 0) {
                    active_child_count--;
                }
                log_event("INFO", "Child %s (PID=%d) terminated cleanly during shutdown.",
                          process_table[idx].name, (int)reaped_pid);
            }
        }

        if (reaped_pid == -1 && errno == ECHILD) {
            break;
        }

        if (count_running_children() == 0) {
            break;
        }

        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        if (get_elapsed_seconds(&start_time, &current_time) >= (double)timeout_sec) {
            break;
        }

        // Wait 100 ms to avoid burning CPU
        struct timespec sleep_ts = {0, 100000000L};
        nanosleep(&sleep_ts, NULL);
    }

    // Phase 3: Force-terminate processes that did not shut down within the timeout ("hang" etc.) with SIGKILL
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (process_table[i].state == PROC_RUNNING && process_table[i].pid > 0) {
            pid_t stubborn_pid = process_table[i].pid;
            log_event("WARN", "Child %s (PID=%d) did not exit within %d seconds. Sending SIGKILL...",
                      process_table[i].name, (int)stubborn_pid, timeout_sec);

            if (kill(stubborn_pid, SIGKILL) == 0 || errno == ESRCH) {
                int status = 0;
                pid_t wp_ret;
                do {
                    wp_ret = waitpid(stubborn_pid, &status, 0);
                } while (wp_ret == -1 && errno == EINTR);

                process_table[i].state = PROC_STOPPED;
                if (wp_ret > 0 && WIFSIGNALED(status)) {
                    process_table[i].last_exit_status = WTERMSIG(status);
                }
                if (active_child_count > 0) {
                    active_child_count--;
                }
                log_event("INFO", "Child %s (PID=%d) terminated via SIGKILL and reaped.",
                          process_table[i].name, (int)stubborn_pid);
            } else {
                log_event("ERROR", "Failed to send SIGKILL to %s (PID=%d): %s",
                          process_table[i].name, (int)stubborn_pid, strerror(errno));
            }
        }
    }

    active_child_count = 0;
    log_event("INFO", "All child processes terminated. Graceful shutdown complete.");
}