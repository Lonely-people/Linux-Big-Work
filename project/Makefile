CC ?= gcc
# 奇奇怪怪的编译选项
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
# 预处理选项  -Iinclude让编译器在include/文件夹里面找对应的.h文件
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
LDFLAGS ?=

BUILD_DIR := build
BIN_DIR := bin
COMMON_OBJECT := $(BUILD_DIR)/protocol.o
LOGGER_OBJECT := $(BUILD_DIR)/logger.o
SESSION_OBJECT := $(BUILD_DIR)/session.o
AI_CLIENT_OBJECT := $(BUILD_DIR)/ai_client.o

.PHONY: all clean test

# 在执行make all的时候，会尝试生成后面跟着的两个目标
all: $(BIN_DIR)/server $(BIN_DIR)/client

test: $(BIN_DIR)/client
	python3 tests/test_client_input.py $(BIN_DIR)/client

# $@ 代表生成目标文件 。$^代表所有的生成依赖文件，不包含|后面的依赖项 。$< 代表第一个依赖项。

# 检查有没有 $(BIN_DIR)这个对象，如果没有，就执行下面的语句
$(BIN_DIR):
	mkdir -p $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/protocol.o: src/protocol.c include/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/server.o: src/server.c include/protocol.h include/logger.h include/session.h include/ai_client.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/client.o: src/client.c include/protocol.h include/client_input.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/client_input.o: src/client_input.c include/client_input.h include/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/logger.o: src/logger.c include/logger.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/session.o: src/session.c include/session.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# 这里需理解ai_client也许调用了session.h里面的函数，但是只是生成.o文件，不需要具体的session.c文件，只需要有函数声明就可以。但是如果是生成可执行文件就不行，必须要包含对应的.c文件。
# 后面的ai_client.o在连接的时候也包含了session.o文件，里面就包含了对应函数的实现
$(BUILD_DIR)/ai_client.o: src/ai_client.c include/ai_client.h include/session.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/server: $(BUILD_DIR)/server.o $(COMMON_OBJECT) $(LOGGER_OBJECT) $(SESSION_OBJECT) $(AI_CLIENT_OBJECT) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

#这里client冒号后面跟着的两个依赖规则仍然会继续尝试生成
$(BIN_DIR)/client: $(BUILD_DIR)/client.o $(BUILD_DIR)/client_input.o $(COMMON_OBJECT) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
