# Watchdog: POSIX Signal-Based Process Monitoring and Management System

Sakarya University — Software Engineering Department  
2026–2027 System Programming Term Project 1

`watchdog` is a POSIX-compliant process supervisor written in C for Linux. It spawns and monitors child (`worker`) processes, automatically restarts unexpectedly terminated processes up to a defined limit, generates on-demand status reports via `SIGUSR1`, and performs a timeout-protected graceful shutdown on `SIGINT` or `SIGTERM`.

## Features

- **Child Process Supervision:** Spawns at least 2 and up to 5 (`MAX_CHILDREN`) child processes using `fork()` and `execv()`. Each worker runs in its own process group (`setpgid`) so terminal `Ctrl+C` signals are handled exclusively by the supervisor.
- **Zombie Prevention & Exit Classification:** Detects child termination via `SIGCHLD` and reaps all exited children in a non-blocking `waitpid(-1, &status, WNOHANG)` loop. Distinguishes normal termination (`exit(0)`) from abnormal exits or terminating signals (`WIFEXITED` / `WIFSIGNALED`).
- **Bounded Automatic Restart:** Automatically restarts crashed or killed workers up to 3 times (`MAX_RESTARTS`). Once the limit is exceeded, the worker is marked as `FAILED_LIMIT` and no further restart attempts are made.
- **Periodic Health Check:** Uses `setitimer(ITIMER_REAL)` to trigger `SIGALRM` every 5 seconds and verify tracked child processes.
- **Async-Signal-Safe Architecture:** Installs all signal handlers (`SIGINT`, `SIGTERM`, `SIGUSR1`, `SIGCHLD`, `SIGALRM`) via `sigaction()`. Handlers only set `volatile sig_atomic_t` flags; actual processing runs in the main loop synchronized with `sigprocmask()` and `sigsuspend()`.
- **On-Demand Status Reporting (`SIGUSR1`):** Logs the ID, name, PID, lifecycle state, restart count, last exit status, mode, and start time of every tracked child process to `watchdog.log`.
- **Graceful Shutdown (`SIGINT` / `SIGTERM`):** Halts new process launches, sends `SIGTERM` to all running children, and waits up to 5 seconds (`DEFAULT_SHUTDOWN_TIMEOUT`) for clean termination. Any unresponsive child remaining after timeout is forcibly terminated with `SIGKILL` and reaped.
- **Timestamped POSIX Logging:** Writes all lifecycle events to both `watchdog.log` and `stdout` using `open(..., O_CLOEXEC)` and `EINTR`-safe `write()` calls.

## Project Structure

```text
.
├── Makefile
├── README.md
├── include/
│   ├── logger.h
│   ├── process_manager.h
│   └── signal_manager.h
├── src/
│   ├── main.c
│   ├── signal_manager.c
│   ├── process_manager.c
│   ├── logger.c
│   └── worker.c
├── workers/
│   └── worker                # Built worker binary
├── tests/
│   └── test_scenarios.sh     # Automated verification script for all 7 scenarios
└── docs/
    └── REPORT.pdf
```

## Prerequisites

- Linux environment (or WSL/Ubuntu on Windows)
- `gcc` (C11 and POSIX.1-2008 support)
- `make`
- `bash` (for running automated tests)

## Build Instructions

Compile the project from the repository root:

```bash
make
```

This compiles all sources with `-Wall -Wextra -Iinclude` and produces:
- `./watchdog` — Main supervisor executable
- `./workers/worker` — Managed test worker executable

Cleanup targets:

```bash
make clean       # Removes object files, watchdog, and workers/worker
make distclean   # Runs clean and removes *.log files
```

## Usage

### 1. Default Mode (2 Normal Workers)

```bash
./watchdog
```

### 2. Custom Number of Normal Workers (2 to 5)

```bash
./watchdog 3
```

### 3. Custom Worker Modes

You can specify execution modes per worker for testing (if fewer than 2 arguments are provided, the supervisor automatically pads with `normal` workers to satisfy the minimum of 2 children):

```bash
./watchdog short normal    # worker_1 exits normally (exit 0), worker_2 keeps running
./watchdog crash normal    # worker_1 crashes repeatedly until hitting the 3-restart limit
./watchdog hang normal     # worker_1 ignores SIGTERM (tests 5s shutdown timeout + SIGKILL)
```

| Worker Mode | Behavior |
| :--- | :--- |
| `normal` | Runs continuously until receiving `SIGTERM`, then exits cleanly. |
| `short` | Runs for 3 seconds and exits normally (`exit(0)`); not restarted. |
| `crash` | Exits unexpectedly (`exit(1)`) after 2 seconds; restarted up to 3 times. |
| `hang` | Ignores `SIGTERM` (`SIG_IGN`); used to verify `SIGKILL` escalation on shutdown timeout. |

### 4. Sending Signals to a Running Watchdog

From another terminal, using the supervisor or verified test worker PID:

```bash
# Trigger process status report in watchdog.log
kill -SIGUSR1 <WATCHDOG_PID>

# Initiate graceful shutdown (or press Ctrl+C in the watchdog terminal)
kill -SIGTERM <WATCHDOG_PID>

# Simulate unexpected worker termination (verify the test worker PID first)
kill -SIGKILL <WORKER_PID>
```

## Testing and Verification

### Automated Test Suite (All 7 Scenarios)

Run the automated test script to build the project and verify all required scenarios (normal operation, normal termination, unexpected termination via `SIGKILL`, 3-restart limit, `SIGUSR1` status reporting, `SIGTERM`/`SIGINT` graceful shutdown with timeout escalation, log format validation, and zombie process check):

```bash
chmod +x tests/test_scenarios.sh
./tests/test_scenarios.sh
```

After execution, the combined log output of all scenarios is stored in `watchdog.log`.

### Manual `Ctrl+C` and Zombie Process Verification

To verify interactive `SIGINT` (`Ctrl+C`) handling separately:

```bash
./watchdog
# Press Ctrl+C while running and observe graceful shutdown logs
# Confirm no defunct (zombie) processes remain:
ps aux | grep defunct
```

## Task Allocation

| Member | Module | Primary Files |
| :--- | :--- | :--- |
| **Student 1** | Signal Management & Main Loop | `src/signal_manager.c`, `src/main.c`, `include/signal_manager.h` |
| **Student 2** | Child Process Management & Worker | `src/process_manager.c`, `src/worker.c`, `include/process_manager.h` |
| **Student 3** | Logging, Build Infrastructure, Tests & Docs | `src/logger.c`, `include/logger.h`, `Makefile`, `tests/test_scenarios.sh`, `README.md` |