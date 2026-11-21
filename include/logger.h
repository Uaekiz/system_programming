#ifndef LOGGER_H
#define LOGGER_H

#define DEFAULT_LOG_FILE "watchdog.log"

// Log dosyasını açar/hazırlar. Başarılıysa 0, hata varsa -1 döner.
int init_logger(const char *log_file_path);

// Açık olan log dosyasını güvenli şekilde kapatır.
void close_logger(void);

// Zaman damgalı olay kaydı yazar (örn: log_event("INFO", "Worker %d started", pid)).
void log_event(const char *level, const char *format, ...);

// SIGUSR1 tetiklendiğinde process_table üzerinden süreç durum raporunu loglar.
void log_process_status_report(void);

#endif // LOGGER_H