#include "logger.h"

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