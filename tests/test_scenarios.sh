#!/usr/bin/env bash
set -u

# Scriptin hangi dizinden calistirildigindan bagimsiz olarak proje kokune gec
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

LOG_FILE="watchdog.log"
COMBINED_LOG="$(mktemp)"
WD_PID=""
PASSED=0
FAILED=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

cleanup() {
    if [[ -n "${WD_PID:-}" ]] && kill -0 "$WD_PID" 2>/dev/null; then
        kill -SIGKILL "$WD_PID" 2>/dev/null || true
        wait "$WD_PID" 2>/dev/null || true
    fi
    rm -f "$COMBINED_LOG"
}
trap cleanup EXIT

pass_case() {
    echo -e "${GREEN}[PASS]${NC} $1"
    PASSED=$((PASSED + 1))
}

fail_case() {
    echo -e "${RED}[FAIL]${NC} $1"
    FAILED=$((FAILED + 1))
}

section() {
    echo -e "\n${BLUE}============================================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}============================================================${NC}"
}

archive_and_reset_log() {
    if [[ -f "$LOG_FILE" ]]; then
        cat "$LOG_FILE" >> "$COMBINED_LOG"
        echo "" >> "$COMBINED_LOG"
    fi
    rm -f "$LOG_FILE"
}

start_watchdog() {
    archive_and_reset_log
    ./watchdog "$@" > /dev/null 2>&1 &
    WD_PID=$!
    sleep 1
    if ! kill -0 "$WD_PID" 2>/dev/null; then
        echo -e "${RED}HATA: watchdog baslatilamadi (args: $*)${NC}"
        return 1
    fi
    return 0
}

stop_watchdog() {
    local sig="${1:-SIGTERM}"
    if [[ -n "$WD_PID" ]] && kill -0 "$WD_PID" 2>/dev/null; then
        kill "-$sig" "$WD_PID" 2>/dev/null || true
        wait "$WD_PID" 2>/dev/null || true
    fi
    WD_PID=""
}

get_latest_worker_pid() {
    local worker_name="$1"
    sed -n "s/.*Started child \[${worker_name}\] (ID=[0-9]*, PID=\([0-9]*\),.*/\1/p" "$LOG_FILE" | tail -n 1
}

wait_for_log_pattern() {
    local pattern="$1"
    local max_sec="$2"
    local elapsed=0
    while (( elapsed < max_sec )); do
        if [[ -f "$LOG_FILE" ]] && grep -qE "$pattern" "$LOG_FILE"; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# 0. Derleme
section "0. Derleme Kontrolu (make clean && make)"
if make clean > /dev/null && make > /dev/null; then
    pass_case "Proje sifir hatayla derlendi (watchdog ve workers/worker hazir)."
else
    fail_case "Derleme basarisiz oldu!"
    exit 1
fi

# 1. Senaryo: Normal Calisma
section "Senaryo 1: Normal Calisma (En Az 2 Alt Surec)"
if start_watchdog; then
    W1_PID="$(get_latest_worker_pid "worker_1")"
    W2_PID="$(get_latest_worker_pid "worker_2")"

    if [[ -n "$W1_PID" && -n "$W2_PID" ]] && kill -0 "$W1_PID" 2>/dev/null && kill -0 "$W2_PID" 2>/dev/null; then
        pass_case "Watchdog (PID=$WD_PID) en az 2 alt sureci baslatti ve takip ediyor (worker_1=$W1_PID, worker_2=$W2_PID)."
    else
        fail_case "Baslangic alt surecleri dogrulanamadi (worker_1=${W1_PID:-yok}, worker_2=${W2_PID:-yok})."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Senaryo 1 baslatilamadi."
fi

# 2. Senaryo: Normal Sonlanma
section "Senaryo 2: Normal Sonlanma (exit(0) - Yeniden Baslatilmamali)"
if start_watchdog short normal; then
    if wait_for_log_pattern "Child \[worker_1\].*exited normally \(status=0\)\. No restart needed\." 6; then
        sleep 1
        if ! grep -q "Restarting child \[worker_1\]" "$LOG_FILE"; then
            W2_PID="$(get_latest_worker_pid "worker_2")"
            if [[ -n "$W2_PID" ]] && kill -0 "$W2_PID" 2>/dev/null; then
                pass_case "worker_1 normal sonlandi, yeniden baslatilmadi; worker_2 (PID=$W2_PID) calismaya devam ediyor."
            else
                fail_case "worker_1 normal sonlandi ancak worker_2 beklenmedik sekilde durdu."
            fi
        else
            fail_case "Normal sonlanan worker_1 hatali olarak yeniden baslatildi."
        fi
    else
        fail_case "worker_1 icin normal sonlanma log kaydi bulunamadi."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Senaryo 2 baslatilamadi."
fi

# 3. Senaryo: Beklenmedik Sonlanma (SIGKILL -> Otomatik Yeniden Baslatma)
section "Senaryo 3: Beklenmedik Sonlanma (kill -SIGKILL -> Otomatik Restart)"
if start_watchdog normal normal; then
    OLD_W1_PID="$(get_latest_worker_pid "worker_1")"
    # Sinyal gondermeden once PID'nin gercekten test worker'ina ait oldugunu dogrula
    if [[ -n "$OLD_W1_PID" ]] && ps -p "$OLD_W1_PID" -o args= 2>/dev/null | grep -q "workers/worker"; then
        kill -SIGKILL "$OLD_W1_PID"
        if wait_for_log_pattern "Restarting child \[worker_1\] \(attempt 1/3\)" 4; then
            sleep 1
            NEW_W1_PID="$(get_latest_worker_pid "worker_1")"
            if grep -q "Child \[worker_1\] (PID=${OLD_W1_PID}) killed by signal 9" "$LOG_FILE" \
               && [[ -n "$NEW_W1_PID" && "$NEW_W1_PID" != "$OLD_W1_PID" ]] \
               && kill -0 "$NEW_W1_PID" 2>/dev/null; then
                pass_case "SIGKILL ile sonlanan worker_1 (eski PID=$OLD_W1_PID) algilandi ve yeni PID=$NEW_W1_PID ile yeniden baslatildi."
            else
                fail_case "worker_1 yeniden baslatma sonrasi yeni PID dogrulanamadi."
            fi
        else
            fail_case "SIGKILL sonrasi yeniden baslatma kaydi gorulmedi."
        fi
    else
        fail_case "worker_1 PID ($OLD_W1_PID) dogrulanamadi, SIGKILL gonderilmedi."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Senaryo 3 baslatilamadi."
fi

# 4. Senaryo: Yeniden Baslatma Limiti (Max 3 Restart + Periyodik Kontrol)
section "Senaryo 4: Yeniden Baslatma Limiti (Maksimum 3 Restart)"
echo -e "${YELLOW}[INFO]${NC} crash modu 4 kez calisip limit asimini tetikliyor (~9-10 sn bekleniyor)..."
if start_watchdog crash normal; then
    if wait_for_log_pattern "Child \[worker_1\] exceeded max restart limit \(3/3\)" 15; then
        sleep 1
        kill -SIGUSR1 "$WD_PID"
        sleep 1

        if grep -q "attempt 1/3" "$LOG_FILE" \
           && grep -q "attempt 2/3" "$LOG_FILE" \
           && grep -q "attempt 3/3" "$LOG_FILE" \
           && ! grep -q "attempt 4/" "$LOG_FILE" \
           && grep -qE "Name=worker_1 .*State=FAILED_LIMIT .*Restarts=3/3" "$LOG_FILE"; then
            pass_case "worker_1 tam 3 kez yeniden baslatildi, 4. kez baslatilmadi ve FAILED_LIMIT olarak raporlandi."
        else
            fail_case "Yeniden baslatma limiti kayitlari veya FAILED_LIMIT durumu eksik."
        fi
    else
        fail_case "worker_1 icin yeniden baslatma limiti asim kaydi zamaninda olusmadi."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Senaryo 4 baslatilamadi."
fi

# 5. Senaryo: Durum Raporlama (SIGUSR1)
section "Senaryo 5: Durum Raporlama (kill -SIGUSR1)"
if start_watchdog normal normal; then
    kill -SIGUSR1 "$WD_PID"
    if wait_for_log_pattern "END OF REPORT \(Active Children: 2\)" 4; then
        if grep -q "PROCESS STATUS REPORT (SIGUSR1)" "$LOG_FILE" \
           && grep -qE "Name=worker_1 \| PID=[0-9]+ \| State=RUNNING" "$LOG_FILE" \
           && grep -qE "Name=worker_2 \| PID=[0-9]+ \| State=RUNNING" "$LOG_FILE"; then
            pass_case "SIGUSR1 sonrasi tum aktif alt sureclerin PID ve durumlari log dosyasina yazildi."
        else
            fail_case "SIGUSR1 raporunda worker PID/durum satirlari eksik."
        fi
    else
        fail_case "SIGUSR1 durum raporu log dosyasinda bulunamadi."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Senaryo 5 baslatilamadi."
fi

# 6a. Senaryo: Kontrollu Kapanma (SIGTERM)
section "Senaryo 6a: Kontrollu Kapanma (SIGTERM)"
if start_watchdog normal normal; then
    W1_PID="$(get_latest_worker_pid "worker_1")"
    W2_PID="$(get_latest_worker_pid "worker_2")"
    stop_watchdog SIGTERM

    if grep -q "Shutdown triggered by SIGTERM" "$LOG_FILE" \
       && grep -q "All child processes terminated. Graceful shutdown complete." "$LOG_FILE" \
       && ! kill -0 "$W1_PID" 2>/dev/null \
       && ! kill -0 "$W2_PID" 2>/dev/null; then
        pass_case "SIGTERM ile tum alt surecler temiz sekilde sonlandirildi ve toplandi."
    else
        fail_case "SIGTERM kontrollu kapanma dogrulamasi basarisiz."
    fi
else
    fail_case "Senaryo 6a baslatilamadi."
fi

# 6b. Senaryo: Kontrollu Kapanma (SIGINT + Timeout Sonrasi SIGKILL)
section "Senaryo 6b: Kontrollu Kapanma (SIGINT + hang Worker Timeout SIGKILL)"
echo -e "${YELLOW}[INFO]${NC} SIGTERM yok sayan hang worker icin 5 sn timeout bekleniyor..."
if start_watchdog hang normal; then
    W1_PID="$(get_latest_worker_pid "worker_1")"
    W2_PID="$(get_latest_worker_pid "worker_2")"
    stop_watchdog SIGINT

    if grep -q "Shutdown triggered by SIGINT" "$LOG_FILE" \
       && grep -q "did not exit within 5 seconds. Sending SIGKILL" "$LOG_FILE" \
       && grep -q "terminated via SIGKILL and reaped." "$LOG_FILE" \
       && grep -q "All child processes terminated. Graceful shutdown complete." "$LOG_FILE" \
       && ! kill -0 "$W1_PID" 2>/dev/null \
       && ! kill -0 "$W2_PID" 2>/dev/null; then
        pass_case "SIGINT ile normal worker temiz kapandi, kapanmayan hang worker timeout sonrasi SIGKILL ile sonlandirildi."
    else
        fail_case "SIGINT veya timeout SIGKILL mekanizmasi dogrulanamadi."
    fi
else
    fail_case "Senaryo 6b baslatilamadi."
fi

# Tum testlerin birlesik logunu watchdog.log icine kaydet
archive_and_reset_log
cp "$COMBINED_LOG" "$LOG_FILE"

# 7. Senaryo: Log ve Zombi Surec Dogrulama
# 7. Senaryo: Log ve Zombi Surec Dogrulama
section "Senaryo 7: Log Format, Olay Kapsami ve Zombi Surec Dogrulama"

INVALID_LINES="$(grep -vE '^$|^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] \[(INFO|WARN|ERROR|STATUS)\] ' "$LOG_FILE" || true)"

if [[ -z "$INVALID_LINES" ]] \
   && grep -q "Watchdog supervisor starting" "$LOG_FILE" \
   && grep -q "Started child" "$LOG_FILE" \
   && grep -q "exited normally" "$LOG_FILE" \
   && grep -q "killed by signal 9" "$LOG_FILE" \
   && grep -q "Restarting child" "$LOG_FILE" \
   && grep -q "exceeded max restart limit" "$LOG_FILE" \
   && grep -q "PROCESS STATUS REPORT (SIGUSR1)" "$LOG_FILE" \
   && grep -q "Periodic health check completed" "$LOG_FILE" \
   && grep -q "Graceful shutdown complete" "$LOG_FILE"; then
    pass_case "Tum kritik olaylar gecerli zaman damgasiyla watchdog.log dosyasinda dogrulandi."
else
    fail_case "Log formatinda veya zorunlu olay kayitlarinda eksiklik var."
fi

ZOMBIES="$(ps -eo pid,stat,comm | awk '$2 ~ /Z/ && ($3 == "worker" || $3 == "watchdog")')"
if [[ -z "$ZOMBIES" ]]; then
    pass_case "Testler sonrasi hic zombi (defunct) watchdog/worker sureci kalmadi."
else
    fail_case "Zombi surec tespit edildi: $ZOMBIES"
fi

echo -e "\n============================================================"
echo -e "TEST OZETI: ${GREEN}${PASSED} BASARILI${NC} | ${RED}${FAILED} BASARISIZ${NC}"
echo -e "Birlesik log dosyasi: ${ROOT_DIR}/${LOG_FILE}"
echo -e "Not: Gercek klavye Ctrl+C (SIGINT) davranisini terminalde './watchdog' ile ayrica dogrulayiniz."
echo -e "============================================================"

if (( FAILED > 0 )); then
    exit 1
fi
exit 0