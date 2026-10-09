# ==============================================================================
# Makefile: Synchronization of Parallel Processes (POSIX & Lock-Free Suite)
# Platform: Linux (Ubuntu 22.04+ / POSIX.1-2008), GCC/G++, POSIX Threads (pthread)
# ==============================================================================

CC       ?= gcc
CXX      ?= g++
CFLAGS   ?= -Wall -Wextra -Werror -pedantic -std=c11 -O2 -pthread -D_GNU_SOURCE
CXXFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c++17 -O2 -pthread
LDFLAGS  ?= -pthread

SRC_DIR  := src
OBJ_DIR  := obj
BIN_DIR  := bin

# Общие исходники
COMMON_SRCS := $(SRC_DIR)/common/cli_parser.c
BUFFER_SRCS := $(SRC_DIR)/prod_cons/buffer.c

COMMON_OBJS := $(OBJ_DIR)/common/cli_parser.o
BUFFER_OBJS := $(OBJ_DIR)/prod_cons/buffer.o

# Список исполняемых файлов
TARGETS := \
    $(BIN_DIR)/prod_cons_naive \
    $(BIN_DIR)/prod_cons_sync \
    $(BIN_DIR)/rw_readers_pref \
    $(BIN_DIR)/rw_writers_pref \
    $(BIN_DIR)/philosophers \
    $(BIN_DIR)/spinlock_test \
    $(BIN_DIR)/aba_demo

.PHONY: all clean run-all benchmark demo help

all: directories $(TARGETS)

directories:
	@mkdir -p $(OBJ_DIR)/common $(OBJ_DIR)/prod_cons $(OBJ_DIR)/readers_writers \
	          $(OBJ_DIR)/philosophers $(OBJ_DIR)/spinlock $(OBJ_DIR)/lockfree $(BIN_DIR)

# Компиляция общих объектных файлов
$(OBJ_DIR)/common/%.o: $(SRC_DIR)/common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/prod_cons/%.o: $(SRC_DIR)/prod_cons/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 1. Продюсер-консьюмер (наивный)
$(BIN_DIR)/prod_cons_naive: $(SRC_DIR)/prod_cons/prod_cons_naive.c $(BUFFER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 2. Продюсер-консьюмер (синхронизированный)
$(BIN_DIR)/prod_cons_sync: $(SRC_DIR)/prod_cons/prod_cons_sync.c $(BUFFER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 3. Читатели-писатели (приоритет читателей)
$(BIN_DIR)/rw_readers_pref: $(SRC_DIR)/readers_writers/rw_readers_pref.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 4. Читатели-писатели (приоритет писателей)
$(BIN_DIR)/rw_writers_pref: $(SRC_DIR)/readers_writers/rw_writers_pref.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 5. Обедающие философы (иерархия Дейкстры)
$(BIN_DIR)/philosophers: $(SRC_DIR)/philosophers/philosophers.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 6. Тестирование Spinlock vs Mutex
$(BIN_DIR)/spinlock_test: $(SRC_DIR)/spinlock/spinlock_test.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 7. Демонстрация проблемы ABA и MS-Queue
$(BIN_DIR)/aba_demo: $(SRC_DIR)/lockfree/aba_demo.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

run-all: all
	@echo "=== 1. НАИВНЫЙ PROD-CONS ==="
	-$(BIN_DIR)/prod_cons_naive --threads 4 --buffer 8 --items 50000
	@echo "\n=== 2. СИНХРОНИЗИРОВАННЫЙ PROD-CONS ==="
	$(BIN_DIR)/prod_cons_sync --threads 4 --buffer 16 --items 100000
	@echo "\n=== 3. ЧИТАТЕЛИ-ПИСАТЕЛИ (READERS PREFERENCE) ==="
	$(BIN_DIR)/rw_readers_pref --threads 4
	@echo "\n=== 4. ЧИТАТЕЛИ-ПИСАТЕЛИ (WRITERS PREFERENCE) ==="
	$(BIN_DIR)/rw_writers_pref --threads 4
	@echo "\n=== 5. ОБЕДАЮЩИЕ ФИЛОСОФЫ ==="
	$(BIN_DIR)/philosophers --threads 5 --items 500
	@echo "\n=== 6. SPINLOCK VS MUTEX ==="
	$(BIN_DIR)/spinlock_test --threads 4 --items 500000
	@echo "\n=== 7. LOCK-FREE ABA DEMO ==="
	$(BIN_DIR)/aba_demo

benchmark: all
	python3 scripts/benchmark.py

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

demo: all
	@bash scripts/run_demo.sh 2>/dev/null || sh scripts/run_demo.sh

help:
	@echo "Доступные цели сборки:"
	@echo "  make all       - собрать все 7 бинарников"
	@echo "  make run-all   - собрать и поочередно запустить демонстрации"
	@echo "  make benchmark - запустить Python-скрипт бенчмаркинга"
	@echo "  make demo      - запустить интерактивное меню сценариев"
	@echo "  make clean     - удалить объектные и исполняемые файлы"
