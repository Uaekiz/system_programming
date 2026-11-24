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
        echo -e "${RED}ERROR: Failed to start watchdog (args: $*)${NC}"
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
section "0. Build Check (make clean && make)"
if make clean > /dev/null && make > /dev/null; then
    pass_case "Project compiled with zero errors (watchdog and workers/worker are ready)."
else
    fail_case "Compilation failed!"
    exit 1
fi

# 1. Senaryo: Normal Calisma
section "Scenario 1: Normal Operation (At Least 2 Child Processes)"
if start_watchdog; then
    W1_PID="$(get_latest_worker_pid "worker_1")"
    W2_PID="$(get_latest_worker_pid "worker_2")"

    if [[ -n "$W1_PID" && -n "$W2_PID" ]] && kill -0 "$W1_PID" 2>/dev/null && kill -0 "$W2_PID" 2>/dev/null; then
        pass_case "Watchdog (PID=$WD_PID) started and is tracking at least 2 child processes (worker_1=$W1_PID, worker_2=$W2_PID)."
    else
        fail_case "Failed to verify initial child processes (worker_1=${W1_PID:-none}, worker_2=${W2_PID:-none})."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Failed to start Scenario 1."
fi

# 2. Senaryo: Normal Sonlanma
section "Scenario 2: Normal Termination (exit(0) - Should Not Restart)"
if start_watchdog short normal; then
    if wait_for_log_pattern "Child \[worker_1\].*exited normally \(status=0\)\. No restart needed\." 6; then
        sleep 1
        if ! grep -q "Restarting child \[worker_1\]" "$LOG_FILE"; then
            W2_PID="$(get_latest_worker_pid "worker_2")"
            if [[ -n "$W2_PID" ]] && kill -0 "$W2_PID" 2>/dev/null; then
                pass_case "worker_1 exited normally and was not restarted; worker_2 (PID=$W2_PID) is still running."
            else
                fail_case "worker_1 exited normally, but worker_2 stopped unexpectedly."
            fi
        else
            fail_case "worker_1 exited normally but was erroneously restarted."
        fi
    else
        fail_case "Normal termination log entry for worker_1 not found."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Failed to start Scenario 2."
fi

# 3. Senaryo: Beklenmedik Sonlanma (SIGKILL -> Otomatik Yeniden Baslatma)
section "Scenario 3: Unexpected Termination (kill -SIGKILL -> Auto Restart)"
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
                pass_case "Detected worker_1 terminated by SIGKILL (old PID=$OLD_W1_PID) and restarted it with new PID=$NEW_W1_PID."
            else
                fail_case "Failed to verify new PID for worker_1 after restart."
            fi
        else
            fail_case "Restart log entry not found after SIGKILL."
        fi
    else
        fail_case "Could not verify worker_1 PID ($OLD_W1_PID); SIGKILL was not sent."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Failed to start Scenario 3."
fi

# 4. Senaryo: Yeniden Baslatma Limiti (Max 3 Restart + Periyodik Kontrol)
section "Scenario 4: Restart Limit (Maximum 3 Restarts)"
echo -e "${YELLOW}[INFO]${NC} Running crash mode 4 times to trigger restart limit (waiting ~9-10 seconds)..."
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
            pass_case "worker_1 was restarted exactly 3 times, not restarted a 4th time, and reported as FAILED_LIMIT."
        else
            fail_case "Restart limit log entries or FAILED_LIMIT state missing."
        fi
    else
        fail_case "Restart limit exceeded log entry for worker_1 did not appear in time."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Failed to start Scenario 4."
fi

# 5. Senaryo: Durum Raporlama (SIGUSR1)
section "Scenario 5: Status Reporting (kill -SIGUSR1)"
if start_watchdog normal normal; then
    kill -SIGUSR1 "$WD_PID"
    if wait_for_log_pattern "END OF REPORT \(Active Children: 2\)" 4; then
        if grep -q "PROCESS STATUS REPORT (SIGUSR1)" "$LOG_FILE" \
           && grep -qE "Name=worker_1 \| PID=[0-9]+ \| State=RUNNING" "$LOG_FILE" \
           && grep -qE "Name=worker_2 \| PID=[0-9]+ \| State=RUNNING" "$LOG_FILE"; then
            pass_case "PIDs and states of all active child processes were written to the log file after SIGUSR1."
        else
            fail_case "Worker PID/state lines missing from SIGUSR1 status report."
        fi
    else
        fail_case "SIGUSR1 status report not found in log file."
    fi
    stop_watchdog SIGTERM
else
    fail_case "Failed to start Scenario 5."
fi

# 6a. Senaryo: Kontrollu Kapanma (SIGTERM)
section "Scenario 6a: Graceful Shutdown (SIGTERM)"
if start_watchdog normal normal; then
    W1_PID="$(get_latest_worker_pid "worker_1")"
    W2_PID="$(get_latest_worker_pid "worker_2")"
    stop_watchdog SIGTERM

    if grep -q "Shutdown triggered by SIGTERM" "$LOG_FILE" \
       && grep -q "All child processes terminated. Graceful shutdown complete." "$LOG_FILE" \
       && ! kill -0 "$W1_PID" 2>/dev/null \
       && ! kill -0 "$W2_PID" 2>/dev/null; then
        pass_case "All child processes were cleanly terminated and reaped on SIGTERM."
    else
        fail_case "SIGTERM graceful shutdown verification failed."
    fi
else
    fail_case "Failed to start Scenario 6a."
fi

# 6b. Senaryo: Kontrollu Kapanma (SIGINT + Timeout Sonrasi SIGKILL)
section "Scenario 6b: Graceful Shutdown (SIGINT + hang Worker Timeout SIGKILL)"
echo -e "${YELLOW}[INFO]${NC} Waiting for 5s timeout for hang worker ignoring SIGTERM..."
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
        pass_case "Normal worker exited cleanly on SIGINT; unresponsive hang worker was terminated via SIGKILL after timeout."
    else
        fail_case "Failed to verify SIGINT or timeout SIGKILL escalation."
    fi
else
    fail_case "Failed to start Scenario 6b."
fi

# Tum testlerin birlesik logunu watchdog.log icine kaydet
archive_and_reset_log
cp "$COMBINED_LOG" "$LOG_FILE"

# 7. Senaryo: Log ve Zombi Surec Dogrulama
section "Scenario 7: Log Format, Event Coverage, and Zombie Process Verification"

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
    pass_case "All critical events verified with valid timestamps in watchdog.log."
else
    fail_case "Invalid log format or missing required event entries."
fi

ZOMBIES="$(ps -eo pid,stat,comm | awk '$2 ~ /Z/ && ($3 == "worker" || $3 == "watchdog")')"
if [[ -z "$ZOMBIES" ]]; then
    pass_case "No zombie (defunct) watchdog/worker processes remain after tests."
else
    fail_case "Zombie process detected: $ZOMBIES"
fi

echo -e "\n============================================================"
echo -e "TEST SUMMARY: ${GREEN}${PASSED} PASSED${NC} | ${RED}${FAILED} FAILED${NC}"
echo -e "Combined log file: ${ROOT_DIR}/${LOG_FILE}"
echo -e "Note: Please also verify interactive Ctrl+C (SIGINT) behavior separately by running './watchdog' in a terminal."
echo -e "============================================================"

if (( FAILED > 0 )); then
    exit 1
fi
exit 0