#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <signal.h>

#define DEFAULT_SHUTDOWN_TIMEOUT 5

// Async-signal-safe signal flags (set to 1 only inside handlers, cleared in the main loop)
extern volatile sig_atomic_t sigchld_flag;
extern volatile sig_atomic_t sigusr1_flag;
extern volatile sig_atomic_t sigalrm_flag;
extern volatile sig_atomic_t shutdown_flag;
extern volatile sig_atomic_t shutdown_signal;

// Installs sigaction() handlers for SIGINT, SIGTERM, SIGUSR1, SIGCHLD and SIGALRM.
// Returns 0 on success, -1 on error.
int setup_signal_handlers(void);

// Sends SIGTERM to active child processes, waits for them to exit within timeout_sec,
// and terminates any that do not exit with SIGKILL, reaping them without leaving zombies.
void graceful_shutdown(int timeout_sec);

#endif // SIGNAL_MANAGER_H