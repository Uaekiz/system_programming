#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <time.h>

#define MAX_CHILDREN 5
#define MAX_RESTARTS 3
#define WORKER_PATH "./workers/worker"

// Lifecycle states of a child process
typedef enum {
    PROC_UNUSED = 0,     // Slot is empty
    PROC_RUNNING,        // Currently running
    PROC_EXITED_NORMAL,  // Terminated normally (exit 0); not restarted
    PROC_CRASHED,        // Crashed due to an error or signal; will be restarted
    PROC_FAILED_LIMIT,   // Exceeded the 3-restart limit; no longer started
    PROC_STOPPED         // Closed via graceful shutdown
} process_state_t;

// Structure representing a single child process
typedef struct {
    int id;                     // Process sequence number (1, 2, ...)
    pid_t pid;                  // Operating system PID value
    process_state_t state;      // Current state
    int restart_count;          // Number of times restarted (max 3)
    int last_exit_status;       // Last exit code or signal number
    time_t start_time;          // Start time
    char name[32];              // Process/worker name (e.g., "worker_1")
    char arg[16];               // Working mode/argument to pass to the worker
} child_process_t;

// Process table also accessible by Person 1 and Person 3
extern child_process_t process_table[MAX_CHILDREN];
extern int active_child_count;

// Core functions to be implemented by Person 2 (you):
void init_process_manager(void);
pid_t start_child_process(int index, const char *name, const char *mode_arg);
void start_initial_children(int count);
void reap_terminated_children(void);       // Reap with waitpid() when SIGCHLD arrives
void check_and_restart_children(void);     // Restart crashed children (within the limit)
void setup_periodic_timer(int seconds);    // setitimer() / SIGALRM setup
void perform_periodic_health_check(void);  // Check to run when SIGALRM fires

#endif // PROCESS_MANAGER_H
