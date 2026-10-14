# 纯 C 终端 Coding Agent

客户端、应用服务器、Agent 循环和测试使用 C11。模型由独立运行的 llama-server 提供；应用通过 libcurl 请求模型，并用五个工具操作实际项目。

本版本位于 `c-agent` 分支。原 C++ 实现保存在 [main 分支](https://github.com/Doubeecat/cpp-coding-agent/tree/main)和 Git 历史中；本分支已移除旧 `.cpp` / `.hpp`、C++ 模板和 nlohmann/json。

```text
终端客户端 ── TCP JSON 帧 ── 应用服务器（每连接 fork）
                                 │
                                 ├── HTTP / SSE ── llama-server
                                 ├── Read / Write / Edit / Glob / Bash
                                 └── 独立工作目录、历史与命令日志
```

## 已完成的重构

- 应用、测试、默认模板和排序演示迁移到 C11，CMake 只启用 C 编译器；JSON 改用仓库内固定的 yyjson 0.13.0。
- 保留五工具、TCP 帧协议、模型 SSE、上下文预算、超时和取消，以及 plain / verbose / JSON / ncurses TUI。
- 拆分 JSON、协议、工作目录、工具、模型、Agent 和界面模块，明确资源所有权、错误传递及释放路径。
- 完善会话恢复和目录锁、日志归属与命令编号；`--resume` 和 `/resume` 恢复旧对话及工具卡片，长历史分块传输。
- C 的核心、集成和 PTY 测试全部通过，ASan/UBSan 通过；本机真实模型的程序生成、自测、并发、会话隔离和断连回收已验收。Linux 尚未实机验证。

## 构建与测试

依赖：C11 编译器、CMake 3.16+、Make、libcurl、ncurses（Linux 需要宽字符版本）。yyjson 已固定版本随仓库保存，正常构建不下载依赖，不需要 C++ 编译器。

```sh
git clone --branch c-agent --single-branch https://github.com/Doubeecat/cpp-coding-agent.git
cd cpp-coding-agent
make build
make test
```

`make test` 运行三个 C 测试组：核心协议与工具、模拟模型和服务端集成、真实 PTY 终端界面。测试会真实编译生成的 C 源码并运行自测，无需模型服务。

检查内存和未定义行为：

```sh
cmake -S . -B build-c-sanitize -DCMAKE_BUILD_TYPE=Debug -DAGENT_SANITIZE=ON
cmake --build build-c-sanitize -j 4
ctest --test-dir build-c-sanitize --output-on-failure
```

简短工作汇总见 [WORK_SUMMARY.md](WORK_SUMMARY.md)，实际验证结果见 [TESTING.md](TESTING.md)，迁移范围与顺序见 [PLAN.md](PLAN.md)。

## 启动

在三个终端分别运行：

```sh
# 终端一：独立模型服务
make run-model

# 终端二：应用服务器
make run-server

# 终端三：客户端，交互终端默认使用 TUI
make run-client
```

模型脚本默认读取上级目录 `Qwen3.5-4B-GGUF/Qwen3.5-4B-Q8_0.gguf`，Mac 使用 Metal，Linux 使用 CPU。可通过 `MODEL_FILE`、`LLAMA_SERVER_BIN`、`MODEL_PORT` 设置路径和端口。模型需要提供 `/props`、`/slots`、`/v1/chat/completions/input_tokens` 和流式 `/v1/chat/completions` 接口。

默认总上下文 65536、两个槽，每槽 32768。应用输入预算 24576、输出 7680、预留 512；默认任务最多 24 轮，模型请求、任务和命令超时分别为 180、900 和 30 秒。超过限制会明确返回并保留历史。

## 使用

默认会话复制 `templates/c` 空项目；模型按照模板说明创建 `src/main.c`，通过 `Bash` 运行 `make test`，实际生成和检查程序。

```sh
build/agent-client --prompt '生成一个从标准输入读取整数并升序输出的 C 程序，包含自测'
build/agent-client --workdir ./test
build/agent-client --plain
build/agent-client --verbose --prompt '检查并测试当前项目'
build/agent-client --json --prompt '介绍当前项目'
```

`--workdir` 直接使用所选目录，保留其文件和构建方式，支持通用项目。C 模板是默认生成语言，既有项目和历史数据不会被强制改写。

TUI 支持中文、多行编辑、括号粘贴、工具卡片和双向鼠标滚动。Enter 发送；Alt+Enter 或 Ctrl+N 换行；Tab 选择工具、Enter 展开；Esc 回到输入；Ctrl+C 退出。粘贴不自动发送，任务运行中可以保留下一轮草稿。

交互命令：

- `/sessions`：列出可恢复会话。
- `/resume ID` 或启动参数 `--resume ID`：恢复历史与工作目录，同时显示旧对话和可展开的工具卡片。
- `/dir`：显示当前目录；`/dir 路径`：首次任务之前或恢复会话后切换目录。
- `/quit`：关闭连接。

工作目录和会话都有独占锁；同一目录或仍活跃的会话无法同时被另一个连接占用。恢复时原目录不可用会明确提示，并继续使用当前目录，日志归属仍跟随被恢复的会话。

恢复历史会分块传输，完整接收后替换当前聊天列表。系统提示不显示在聊天中；工具卡片保留当时的参数、结果和日志路径。升级后需要重新启动应用服务器和客户端，模型服务可以继续运行。

## 工具与结果

| 工具 | 行为 |
| --- | --- |
| Read | 按行读取，支持 offset / limit；反馈最多 4 KiB |
| Write | 创建或完整替换文件，最多 64 KiB，保留原有权限并原子替换 |
| Edit | 精确替换原内容；多处匹配需 replace_all=true |
| Glob | 支持 `*`、`?`、字符组和跨目录的 `**`；结果排序并限量 |
| Bash | 在当前目录执行命令，stdin 为 EOF；实时输出，返回退出码、超时和日志路径 |

文件工具限制在工作目录内，拒绝跨目录和符号链接访问。Bash 执行普通本机命令，工作目录隔离不构成系统沙箱。

`task_done.status=success` 表示 Agent 正常完成本轮对话；某条命令的成功与否由其 `tool_result.result.ok` 和 `exit_code` 表示。程序不会因为模型声称通过而伪造测试结果，也不强制把所有任务套入固定构建规则。

TCP 使用 4 字节网络字节序长度 + UTF-8 JSON，单帧最多 1 MiB，JSON 嵌套最多 128 层。会话存放在 `sessions/<ID>/`：`history.jsonl`、`events.jsonl`、`.workspace`、`command-N.log` 和默认工作目录。恢复及目录切换保留旧命令日志。既有会话中的 C++ 用户文件属于历史数据，不自动删除。

## 代码组织

- `common.h`：配置、错误结构、回调和公共接口。
- `json.c` / `util.c` / `protocol.c`：JSON 所有权、缓冲区、时间、路径及帧收发。
- `workspace.c` / `tools.c`：工作目录、历史恢复和五个工具。
- `model_client.c` / `agent.c` / `server.c`：HTTP/SSE、模型与工具循环、连接进程管理。
- `client_ui.c` / `client_tui.c` / `client.c`：事件状态、终端布局与输入、CLI。

函数返回值与 `CaError` 传递错误；借用 JSON 值仅在所属文档存活期间使用，需要保存时复制。资源在明确的清理路径释放。详细流程见 [AGENT.md](AGENT.md)。
