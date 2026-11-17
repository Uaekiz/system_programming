#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <time.h>

#define MAX_CHILDREN 5
#define MAX_RESTARTS 3
#define WORKER_PATH "./workers/worker"

// Alt sürecin yaşam döngüsü durumları
typedef enum {
    PROC_UNUSED = 0,     // Yuva boş
    PROC_RUNNING,        // Şu an çalışıyor
    PROC_EXITED_NORMAL,  // Normal şekilde (exit 0) sonlandı, yeniden başlatılmaz
    PROC_CRASHED,        // Hata veya sinyalle çöktü, yeniden başlatılacak
    PROC_FAILED_LIMIT,   // 3 yeniden başlatma sınırını aştı, artık başlatılmaz
    PROC_STOPPED         // Graceful shutdown ile kapatıldı
} process_state_t;

// Her bir alt süreci temsil eden yapı
typedef struct {
    int id;                     // Süreç sıra numarası (1, 2, ...)
    pid_t pid;                  // İşletim sistemi PID değeri
    process_state_t state;      // Güncel durumu
    int restart_count;          // Kaç kez yeniden başlatıldığı (max 3)
    int last_exit_status;       // Son çıkış kodu veya sinyal numarası
    time_t start_time;          // Başlatıldığı zaman
    char name[32];              // Süreç/worker adı (örn: "worker_1")
    char arg[16];               // Worker'a verilecek çalışma modu/argümanı
} child_process_t;

// Kişi 1 ve Kişi 3'ün de erişebileceği süreç tablosu
extern child_process_t process_table[MAX_CHILDREN];
extern int active_child_count;

// Kişi 2'nin (Senin) kodlayacağı temel fonksiyonlar:
void init_process_manager(void);
pid_t start_child_process(int index, const char *name, const char *mode_arg);
void start_initial_children(int count);
void reap_terminated_children(void);       // SIGCHLD geldiğinde waitpid() ile toplama
void check_and_restart_children(void);     // Çökenleri (limit dahilinde) yeniden başlatma
void setup_periodic_timer(int seconds);    // setitimer() / SIGALRM kurulumu
void perform_periodic_health_check(void);  // SIGALRM tetiklenince yapılacak kontrol

#endif // PROCESS_MANAGER_H
