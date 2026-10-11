# C++ 极简 Coding Agent

一个面向课程作业的本地编程助手：在终端提出需求，助手生成或修改 C++ 代码，写入文件、编译和运行自测，并根据错误继续修复。下一轮可以继续修改刚才生成的程序。服务端使用 `fork()` 同时服务多个客户端，各自拥有独立的对话历史和日志；工作目录可由客户端指定。

客户端、应用服务器、Agent 和验收程序均使用 C++17。通过本机 llama.cpp 运行已有的 Qwen3.5-4B-Q8_0，不调用外部大模型 API。开发和验收全部在 macOS 本机完成，无需 Docker。

## 构建

需要 C++17 编译器、CMake、make、libcurl、支持宽字符的 ncurses 和支持本模型的 llama-server。Mac 上 libcurl 与 ncurses 使用系统 SDK；Linux 上需安装对应的开发包（如 `libcurl4-openssl-dev`、`libncurses-dev`）。nlohmann/json 3.11.3 已附带，构建时无需下载。

在本目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

现有机器已安装这些工具。新 Mac 如果缺少编译器，可安装 Xcode Command Line Tools；缺少 CMake/llama.cpp 时可通过 Homebrew 安装。

## 启动：三个终端

终端一启动模型：

```bash
cd /Users/catcat/Documents/FZU/linux/coding-agent
./scripts/start-model.sh
```

脚本引用上级目录里已有的权重，默认在 Mac 上使用 Metal。模型只加载一份，总上下文 16384，两个槽各 8192；关闭 thinking 和自动上下文移位。

终端二启动 C++ 应用服务器：

```bash
cd /Users/catcat/Documents/FZU/linux/coding-agent
./build/agent-server
```

终端三启动客户端：

```bash
cd /Users/catcat/Documents/FZU/linux/coding-agent
./build/agent-client
```

输入下面两个任务，每个任务结束后继续输入下一条：

```text
从零生成一个 C++ 命令行排序程序：从标准输入读取整数，升序输出，支持负数和重复数字。添加正常和空输入的自测，实际写入文件并编译测试。
给刚才的程序增加 --descending 参数，支持降序，保留默认升序行为，补充自测并运行。
```

交互终端默认进入 TUI，模型回答实时出现，工具调用默认折叠。`/quit`、输入为空时 Ctrl+D 或 Ctrl+C 退出；生成中关闭客户端，其他客户端继续工作。模型和服务端在各自终端按 Ctrl+C 停止。

再开一个终端运行 `./build/agent-client` 即可测试两个客户端。每个新连接从 `templates/cpp/` 复制一个空项目，最初没有应用源码；模型根据需求创建 `src/main.cpp`，也可以创建其他 `.cpp` / 头文件。不同连接可生成不同程序。

## 终端界面与指定目录

直接在指定目录生成或修改代码，也支持相对路径：

```bash
./build/agent-client --workdir ./my-project
```

最下方固定显示当前工作目录、已测量的上下文 tokens / 本连接上限、模型名称，例如：

```text
目录 …/my-project | Token 1234/8192 | 模型 qwen3.5-4b
```

tokens 包含系统提示词、工具定义、对话和工具调用，使用本地 llama.cpp 的 tokenizer 与流式响应 usage。每次模型请求开始、结束时更新；首次提问前显示 `—`。总上下文 16K 分配给两个并行槽，因此默认这里显示每连接 8192。输入预算仍是 5632，其余留给生成和预留空间。长路径会保留末尾，窗口缩放后状态栏仍位于底部。

| 操作 | 效果 |
| --- | --- |
| 点击工具标题 | 展开 / 收起参数、完整源码、读取结果或构建输出 |
| Tab / Shift+Tab | 选中工具；首次选中最近一次调用 |
| ↑ / ↓ | 输入区上下移动光标；工具选中时切换工具 |
| Enter | 输入区发送；工具选中时展开 / 收起 |
| Alt+Enter / Ctrl+N | 在输入中换行 |
| Esc 或点击输入区 | 返回输入框；点击可定位光标 |
| 鼠标滚轮、PgUp / PgDn | 滚动会话；位于超长输入区时滚动输入 |
| Ctrl+U | 清空输入 |

输入框按宽度自动换行，随内容增加高度。多行粘贴会完整保留在草稿中，按 Enter 才发送；内容超过屏幕时显示输入行范围，滚动即可查看整段。发送后的完整 prompt 保留在会话中。生成过程中可以输入下一轮草稿，待当前任务结束后再按 Enter 发送。

工具展开后显示完整的参数与结果 JSON，保留 `content`、`output` 和嵌套字段，并提供源码与实时构建输出。长 JSON 自动换行，可用滚轮上下查看；右侧滚动条和顶部行范围表示当前位置。鼠标同时支持现代和旧版终端报告格式，包含 Mac 上的向下滚动。工具本身的单次读取、模型反馈预算见下方工具说明。

顶部显示旋转提示和“思考中 / 生成中 / 工具执行中 / 编译测试中”，每 0.1 秒更新本轮实际耗时；结束后显示完成状态及总用时。等待期间即使模型没有新输出，计时仍会更新。鼠标依赖终端开启鼠标报告，键盘操作也可完成展开和滚动。

`--workdir DIR` **直接读写指定目录**，不会复制其中的代码。不存在的目录会创建；缺失的 `Makefile` 和 `README.md` 从模板补齐，并创建 `src/`。已有文件保留，Agent 仍只能修改 `src/`。同一个目录只允许一个活跃 Agent 连接；退出后释放目录锁，可重新连接继续修改原有文件。新连接的对话历史重新开始，日志仍分别存入 `sessions/<session_id>/`。目录在第一次提问前确定，运行中更换目录需重新启动客户端。

选择目录只影响 Agent 的项目文件读写，客户端 shell 的当前目录保持原样。路径由客户端转换为绝对路径后交给服务端，项目针对同一台机器上的客户端与服务端设计。

`--plain` 使用简略文本界面，`--verbose` 使用文本界面并完整打印工具内容，`--json` 输出完整 JSON 事件。带 `--prompt`、输入输出重定向或 `TERM=dumb` 时自动使用文本模式。

模型默认端口为 8080，应用默认端口为 8888。端口被占用时不会自动终止已有进程，可改用：

```bash
# 模型终端
MODEL_PORT=8081 ./scripts/start-model.sh
# 服务端终端
./build/agent-server --port 8889 --model-url http://127.0.0.1:8081
# 客户端终端
./build/agent-client --port 8889
```

## 非交互演示与验收

用一个连接连续完成两轮任务：

```bash
./build/agent-client \
  --prompt '从零生成一个 C++ 命令行排序程序：从标准输入读取整数，升序输出，支持负数和重复数字。添加正常和空输入的自测，实际写入文件并编译测试。' \
  --prompt '给刚才的程序增加 --descending 参数，支持降序，保留默认升序行为，补充自测并运行。'
```

加 `--json` 可输出逐行 JSON 事件，便于检查时间、会话及任务状态。客户端退出码为 0 表示本连接各轮都成功，任意一轮失败则为 1。

`ctest` 使用 C++ 模拟模型验证协议、文件边界、真实构建、并发隔离、断连和异常处理，不需要启动模型。

模型和应用服务已启动后，还可以运行纯 C++ 的真实模型验收：

```bash
./build/agent-live-check
# 自定义应用端口时：./build/agent-live-check 8889
```

它会检查两个连接同时流式生成、连续提问时记住各自代号，以及关闭 A 后 B 继续生成。原始事件和汇总保存在 `runtime/live-*-*.jsonl` / `runtime/live-*-summary.json`。真实编程能力与这项并发验收分开验证；本次记录见 [TESTING.md](TESTING.md)。

## 文件与工具

每个连接生成：

```text
sessions/<session_id>/
├── workspace/       # 未指定 --workdir 时使用的项目副本
├── history.jsonl    # 本连接完整消息、工具调用及结果
├── events.jsonl     # 流式事件、状态、时间、request_id
└── build-N.log      # 每次构建的完整输出
```

客户端欢迎消息显示实际工作目录，可以查看生成的源码，并执行其中的 `workspace/build/app`。上面的排序需求可以在该工作目录下这样运行：

```bash
printf '3 -1 3 0\n' | ./build/app
printf '3 -1 3 0\n' | ./build/app --descending
./build/app --self-test
```

断开连接后文件仍保留。不指定工作目录时，新连接使用新的空项目；指定原来的目录可以继续编辑已有代码。

| 工具 | 功能 |
| --- | --- |
| `list_files()` | 列出项目文件 |
| `read_file(path, start, count)` | 带行号读取；每次最多 100 行 / 4 KiB |
| `write_file(path, content)` | 创建或完整替换 `src/` 下的源文件，最多 64 KiB |
| `build_and_test()` | 执行固定的 `make test`，编译全部 `src/*.cpp`，运行 `build/app --self-test` |

Makefile 由模板提供且只读。生成的程序需要支持 `--self-test`：测试实际功能和边界，打印结果，通过返回 0、失败返回非 0，不等待输入。自测也保存在 `src/`，由模型编写和维护；构建启用 UBSan 和浮点转整数溢出检查，运行时将标准输入设为空。其他自定义模板的外部验收测试仍不可由工具修改。

Agent 只能写 `src/`。路径检查拒绝绝对路径、`..` 和符号链接；不同工作区用于会话隔离，不是运行任意不可信代码的操作系统沙箱。

成功状态要求最后一次源文件修改之后的测试通过。成功构建后，模型的下一轮只允许生成总结；仅剩总结时若预算或轮数耗尽，直接返回已验证的测试结果。如果模型提前声称完成或代码仍未通过测试，服务端会明确返回失败状态。

`tests_passed` 表示固定构建命令实际成功退出。自测由模型生成，所以它不能独立证明全部需求正确；可以查看源码、自测覆盖范围和程序的实际输出，再提出补充需求。

## 16K 预算与可配置项

两个并行槽各使用 8K。默认每次完整输入最多 5632 tokens，生成最多 2048，预留 512；通过本地 `/v1/chat/completions/input_tokens` 对整个请求计数，包含工具定义和历史。该接口在本机 llama.cpp 中已实测可用。生成整份源码需要比小幅修改更大的输出预算，输入预算已相应减少，总上下文仍为 16K。

达到预算时提示新建连接，历史和日志保留。首版不删除旧消息或自动摘要。单任务最多 8 次模型请求，单次请求 180 秒，构建测试 30 秒，总任务 8 分钟。小模型有时需要更多修复轮次，可显式调整：

```bash
./build/agent-server --max-rounds 12
./build/agent-server --help
```

`--project DIR` 可以指定另一个小型 C++ 模板，要求提供 `src/` 和受信任的 `Makefile`，其中 `make test` 必须以退出码反映测试成败。`--sessions DIR` 指定会话文件存储位置。服务端的模型地址只接受本机 HTTP 地址。

## 源码与作业要点

- `server.cpp`：`socket/bind/listen/accept`、每连接 `fork`、`waitpid` 回收。
- `protocol.cpp`：长度前缀 JSON 协议、短读短写、UTF-8、连接关闭。
- `agent.cpp`：本连接消息历史、token 预算、工具调用循环及状态。
- `model_client.cpp`：libcurl、增量 SSE、完整工具参数组装与取消。
- `tools.cpp`：文件读写、原子替换、`pipe/fork/exec` 构建、超时及进程组清理。
- `client.cpp`：命令行参数、指定目录、TUI / 文本 / JSON 模式选择。
- `client_tui.cpp`：ncurses 界面、固定状态栏、工具折叠、鼠标与宽字符输入；增量解析网络消息以保持界面响应。

`agent.cpp` 有两层循环：外层等待当前连接的新问题，内层反复请求模型、执行工具、把结果加入历史。文本直接流式发送；工具请求先完整收齐再执行。模型负责提出操作，C++ 程序负责执行、记录和根据真实构建结果判定状态。

按函数、回调与状态变量的讲解见 [AGENT.md](AGENT.md)。

报告可以围绕“本地模型驱动的小型代码生成系统”展开，展示需求 → 创建源码 → 编译自测 → 错误反馈 → 修复，以及两个终端的并发输出和独立日志。模型生成质量不作稳定性承诺，建议一次提出一个小功能，失败结果同样会完整记录。
