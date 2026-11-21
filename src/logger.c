#include "logger.h"
#include "process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>


static int log_fd = -1;

// Kısmi yazma ve sinyal kesintilerine (EINTR) dayanıklı POSIX write sarmalayıcısı
static ssize_t write_all(int fd, const char *buf, size_t len) {
    size_t total_written = 0;

    while (total_written < len) {
        ssize_t written = write(fd, buf + total_written, len - total_written);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total_written += (size_t)written;
    }
    return (ssize_t)total_written;
}


static const char *state_to_string(process_state_t state) {
    switch (state) {
        case PROC_UNUSED:        return "UNUSED";
        case PROC_RUNNING:       return "RUNNING";
        case PROC_EXITED_NORMAL: return "EXITED_NORMAL";
        case PROC_CRASHED:       return "CRASHED";
        case PROC_FAILED_LIMIT:  return "FAILED_LIMIT";
        case PROC_STOPPED:       return "STOPPED";
        default:                 return "UNKNOWN";
    }
}

int init_logger(const char *log_file_path) {
    const char *path = (log_file_path != NULL && log_file_path[0] != '\0')
                           ? log_file_path
                           : DEFAULT_LOG_FILE;

    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }

    log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) {
        perror("init_logger: open failed");
        return -1;
    }

    return 0;
}

void close_logger(void) {
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
}

void log_event(const char *level, const char *format, ...) {
    if (log_fd < 0) {
        // init_logger çağrılmadan log_event kullanılırsa varsayılan dosyayı otomatik aç
        init_logger(DEFAULT_LOG_FILE);
    }

    time_t now = time(NULL);
    struct tm tm_info;
    char timestamp[32];

    if (localtime_r(&now, &tm_info) != NULL) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
    } else {
        snprintf(timestamp, sizeof(timestamp), "0000-00-00 00:00:00");
    }

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[1200];
    int line_len = snprintf(line, sizeof(line), "[%s] [%s] %s\n",
                            timestamp,
                            (level != NULL) ? level : "INFO",
                            message);

    if (line_len <= 0) {
        return;
    }

    size_t bytes_to_write = (size_t)line_len;
    if (bytes_to_write >= sizeof(line)) {
        bytes_to_write = sizeof(line) - 1;
        line[bytes_to_write - 1] = '\n';
    }

    if (log_fd >= 0) {
        write_all(log_fd, line, bytes_to_write);
    }

    // Canlı takip ve test kolaylığı için terminale de yaz
    write_all(STDOUT_FILENO, line, bytes_to_write);
}


void log_process_status_report(void) {
    log_event("STATUS", "========== PROCESS STATUS REPORT (SIGUSR1) ==========");

    int tracked_count = 0;

    for (int i = 0; i < MAX_CHILDREN; i++) {
        const child_process_t *proc = &process_table[i];

        if (proc->state == PROC_UNUSED) {
            continue;
        }

        tracked_count++;

        char start_buf[32] = "N/A";
        if (proc->start_time > 0) {
            struct tm start_tm;
            if (localtime_r(&proc->start_time, &start_tm) != NULL) {
                strftime(start_buf, sizeof(start_buf), "%H:%M:%S", &start_tm);
            }
        }

        log_event("STATUS",
                  "ID=%d | Name=%s | PID=%d | State=%s | Restarts=%d/%d | LastExit=%d | Mode=%s | Started=%s",
                  proc->id,
                  proc->name,
                  (int)proc->pid,
                  state_to_string(proc->state),
                  proc->restart_count,
                  MAX_RESTARTS,
                  proc->last_exit_status,
                  proc->arg,
                  start_buf);
    }

    if (tracked_count == 0) {
        log_event("STATUS", "No child processes currently tracked in process_table.");
    }

    log_event("STATUS", "====== END OF REPORT (Active Children: %d) ======", active_child_count);
}