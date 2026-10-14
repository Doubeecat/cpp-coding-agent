# 顶层构建入口：把 CMake 构建、CTest、模型服务和客户端包成简单目标。
# 依赖：CMake、C11 编译器、libcurl、ncurses、llama-server 和本机 Qwen3.5-4B-Q8_0 权重。
BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 4
PORT ?= 8888
MODEL_PORT ?= 8080
SERVER := $(BUILD_DIR)/agent-server
CLIENT := $(BUILD_DIR)/agent-client
LIVE_CHECK := $(BUILD_DIR)/agent-live-check

.PHONY: all build test rebuild run-model run-server run-client run-live-check clean help

all: build

## 配置并编译全部目标
build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j $(JOBS)

## 运行完整 CTest（使用 C 模拟模型，不需要启动模型服务）
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

## 删除构建目录后重新编译
rebuild:
	rm -rf $(BUILD_DIR)
	$(MAKE) build

## 终端一：启动本机模型服务（每槽 32768 tokens，两个并行槽）
run-model:
	./scripts/start-model.sh

## 终端二：启动应用服务器
run-server: build
	$(SERVER) --port $(PORT) --model-url http://127.0.0.1:$(MODEL_PORT)

## 终端三、终端四：启动客户端（--workdir 指定已有项目目录）
run-client: build
	$(CLIENT) --port $(PORT) $(CLIENT_ARGS)

## 模型与应用服务已启动时，运行真实模型验收
run-live-check: build
	$(LIVE_CHECK) $(PORT)

clean:
	rm -rf $(BUILD_DIR) build-sanitize

help:
	@echo "make build          配置并编译（默认 Release，可用 BUILD_TYPE=Debug）"
	@echo "make test           编译后运行 CTest"
	@echo "make rebuild        删除 $(BUILD_DIR) 后重新编译"
	@echo "make run-model      启动 llama-server（终端一）"
	@echo "make run-server     启动应用服务器（终端二）"
	@echo "make run-client     启动客户端（终端三/四，可加 CLIENT_ARGS='--workdir DIR'）"
	@echo "make run-live-check 运行真实模型并发与隔离验收"
	@echo "make clean          删除构建目录"
