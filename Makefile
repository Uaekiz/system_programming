CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

TARGET = watchdog
WORKER_TARGET = workers/worker

SRCS = src/main.c src/signal_manager.c src/process_manager.c src/logger.c
OBJS = $(SRCS:.c=.o)
HEADERS = include/process_manager.h include/signal_manager.h include/logger.h

.PHONY: all clean distclean

all: $(TARGET) $(WORKER_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(WORKER_TARGET): src/worker.c
	@mkdir -p workers
	$(CC) $(CFLAGS) -o $@ $<

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(WORKER_TARGET)

distclean: clean
	rm -f *.log