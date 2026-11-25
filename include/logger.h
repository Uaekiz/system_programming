#ifndef LOGGER_H
#define LOGGER_H

#define DEFAULT_LOG_FILE "watchdog.log"

// Opens/prepares the log file. Returns 0 on success, -1 on error.
int init_logger(const char *log_file_path);

// Safely closes the open log file.
void close_logger(void);

// Writes a timestamped event entry (e.g., log_event("INFO", "Worker %d started", pid)).
void log_event(const char *level, const char *format, ...);

// Logs a process status report from process_table when SIGUSR1 is triggered.
void log_process_status_report(void);

#endif