# 纯 C 重构计划

2026-10-14；基线提交 `58ba64f`。用户已确认：应用、测试、默认模板和演示使用 C，保留独立的 llama-server。

## 当前行为与目标

迁移前应用为 C++17，使用 nlohmann/json、libcurl、ncurses。7 个实现文件和 2 个头文件约 2261 行，测试约 1098 行。原计划和 AGENT.md 已落后于源码；迁移以实际源码和测试为准。

保留 TCP 长度帧协议、每连接 fork、五工具 Read/Write/Edit/Glob/Bash、会话列表与恢复、工作目录锁、模型 SSE、token 预算、plain/verbose/JSON/TUI、中文多行编辑、工具卡片、粘贴和鼠标滚动。工具仍支持通用工作目录及可编辑构建规则；task_done.success 表示 Agent 正常结束，命令成功与否由工具退出码表示。

默认单槽上下文 32768，输入 24576、输出 7680、预留 512，最多 24 轮，模型/任务/命令超时分别为 180/900/30 秒。

原 C++ 基线已执行 `make test BUILD_DIR=build-c-baseline`：CTest 1/1 通过，15.23 秒。保留忽略目录中的旧二进制供对照。

## 技术约定

- C11 + POSIX；CMake 最终仅启用 LANGUAGES C，全部应用、测试和模板使用 C 编译器。
- 固定并随仓库保存 [yyjson 0.13.0](https://github.com/ibireme/yyjson/releases/tag/0.13.0) 及 MIT 许可证和 SHA-256。其严格 UTF-8、64 位整数和带长度字符串接口用于保留 JSON 行为，见[官方说明](https://github.com/ibireme/yyjson)。普通构建不联网下载。
- JSON 访问结果为借用值，跨事件保存时复制；文档统一释放。缓冲区记录长度与容量，检查分配失败和整数溢出。路径与命令拒绝内嵌 NUL，文件内容使用显式长度。
- 返回值与错误结构替代异常，函数指针 + context 替代闭包。回调能传播日志、发送和解析错误。资源结构提供 init/destroy，所有失败路径统一清理。
- 单调时钟用于超时，实时时钟用于日志。保留信号、进程组终止与回收、原子文件替换、权限与符号链接检查。
- 保留事件类型、字段、顺序及历史格式；JSON 比较按结构，忽略键顺序和动态时间/PID/会话 ID。

## 实施顺序与验收

1. 底层：common.h、util.c、json.h/json.c、protocol.c。验证短读短写、EOF、非法帧、分包粘包和 UTF-8 分块。
2. 工作目录与工具：workspace.c、tools.c；建立 templates/c 和 C 源码夹具。验证目录锁、历史恢复、路径边界、原子写、精准替换、globstar、真实编译、自测、超时与取消。
3. 模型与服务端：model_client.c、agent.c、server.c。用模拟 HTTP/SSE 验证工具增量、历史、截断不执行、上下文/轮次限制、并发与断连。
4. 客户端：client.c、client_ui.h/client_ui.c、client_tui.c。保留 CLI、会话命令、界面状态及完整 TUI；通过 PTY 验证中文、粘贴、缩放、卡片和滚动。
5. 收尾：tests 全部改为 C，迁移 live-check 与演示；移除应用所需的 C++ 和 nlohmann，更新构建入口、README、AGENT.md 及实际验收记录。

特别回归会话切换后的日志归属、命令日志不覆盖、原目录恢复失败时的回退状态、当前会话 ID，以及取消/回调失败时的资源释放。

最终要求：全新 C11 构建、完整确定性测试通过、AddressSanitizer/UndefinedBehaviorSanitizer 检查通过；默认模板真实生成、编译、自测和连续修改；受版本管理的应用/测试/模板/演示不依赖 .cpp/.hpp 或 C++ 标准库。真实模型检查单独记录，只标记实际运行过的结果；未运行的 Linux 验证明确列为待验。既有会话数据保留。

## 执行状态

- [x] 源码阅读、范围确认、原 C++ 基线测试。
- [x] 重构计划。
- [x] 底层接口。
- [x] 工作目录、持久化与五工具。
- [x] 模型、Agent、服务端。
- [x] 客户端与完整 TUI。
- [x] C 测试、模板、演示、文档与最终验收。

2026-10-14 已完成：默认构建在 `CXX=/nonexistent` 下通过全部三个 C 测试组，ASan/UBSan 通过；真实 Qwen 模型的并发、历史隔离、断连回收和 C 程序生成均已实测。验收命令、覆盖范围及 Linux 未实测说明见 [TESTING.md](TESTING.md)。
