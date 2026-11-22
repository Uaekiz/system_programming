#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <signal.h>

#define DEFAULT_SHUTDOWN_TIMEOUT 5

// Async-signal-safe sinyal bayrakları (yalnızca handler içinde 1 yapılır, ana döngüde sıfırlanır)
extern volatile sig_atomic_t sigchld_flag;
extern volatile sig_atomic_t sigusr1_flag;
extern volatile sig_atomic_t sigalrm_flag;
extern volatile sig_atomic_t shutdown_flag;
extern volatile sig_atomic_t shutdown_signal;

// SIGINT, SIGTERM, SIGUSR1, SIGCHLD ve SIGALRM için sigaction() kurulumu yapar.
// Başarılıysa 0, hata durumunda -1 döner.
int setup_signal_handlers(void);

// Aktif alt süreçlere SIGTERM gönderir, timeout_sec süresi içinde kapanmalarını bekler,
// kapanmayanları SIGKILL ile sonlandırıp zombi bırakmadan toplar.
void graceful_shutdown(int timeout_sec);

#endif // SIGNAL_MANAGER_H