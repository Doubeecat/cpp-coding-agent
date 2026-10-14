# 纯 C 重构验收记录

日期：2026-10-14。环境：macOS arm64，Apple Clang 21.0.0。应用、测试、默认模板和演示均使用 C11；llama-server 保持独立运行。

## 构建与确定性测试

下表记录纯 C 迁移完成时的结果；后续恢复界面修复的结果见下一节。

| 检查 | 实际结果 |
| --- | --- |
| 原 C++ 基线（提交 `58ba64f`） | CTest 1/1 通过，15.23 秒 |
| 全新 Release：`env CXX=/nonexistent make test BUILD_DIR=build-c-release` | CTest 3/3 通过，不使用 C++ 编译器 |
| 最终 Release：`make test BUILD_DIR=build-c-release` | CTest 3/3 通过，11.66 秒 |
| 默认入口：`env CXX=/nonexistent make test` | CTest 3/3 通过，9.89 秒；`build/` 已更新为 C 二进制 |
| 最终 Debug + AddressSanitizer/UndefinedBehaviorSanitizer | CTest 3/3 通过，20.95 秒，未报告内存或未定义行为错误 |
| C 演示：`make -C test test` | 真实排序自测通过；输入 `5\n3 1 4 1 2\n` 输出 `1 1 2 3 4` |
| yyjson 固定依赖 | `shasum -a 256 -c SHA256SUMS` 三项均通过 |
| 源码与链接检查 | 应用、测试、模板、演示及第三方源码无 `.cpp` / `.hpp`；客户端和服务器无直接 C++ 标准库链接 |
| 格式检查 | 项目 C 源码通过 clang-format 检查，`git diff --check` 通过 |

构建和测试命令：

```sh
env CXX=/nonexistent make test

cmake -S . -B build-c-sanitize -DCMAKE_BUILD_TYPE=Debug -DAGENT_SANITIZE=ON
cmake --build build-c-sanitize -j 4
ctest --test-dir build-c-sanitize --output-on-failure

make -C test test
(cd third_party/yyjson && shasum -a 256 -c SHA256SUMS)
```

## 恢复会话界面修复

同日修复了恢复后旧对话未显示的问题。默认 Release 完整测试 3/3 通过（18.99 秒），ASan/UBSan 完整测试 3/3 通过（24.43 秒）。`build/` 中的应用服务器和客户端已重新编译。

新增验证覆盖：

- 在真实 PTY 中通过启动参数 `--resume` 和交互命令 `/resume` 显示旧用户消息、助手回复及可展开的工具卡片，随后继续提问。
- 超过 1 MiB 的 Unicode 历史分块传输、完整还原，并在 plain 模式显示；模型继续收到旧上下文。
- 会话切换替换旧视图；工具结果按调用 ID 和助手轮次匹配，保留真实失败结果、原始参数及目录恢复警告。
- 接收期间保留输入草稿，完整接收后提交新视图；坏快照保留原有内容；旧服务端缺少历史数据时提示重启并解除等待状态。

## 三个 C 测试组

- **core**：JSON 所有权、64 位整数、内嵌 NUL、重复键、非法 UTF-8、分包粘包与 EOF、SSE 分块、目录锁与路径边界、五工具、真实 C 编译和自测、失败退出码、分数秒超时、取消与进程清理、日志编号、模板权限与不覆盖文件、历史恢复及重复工具 ID 的中断修复。
- **integration**：C 模拟 HTTP/SSE 服务配合真实服务器和客户端，覆盖工具参数增量、生成与连续修改 C 程序、上下文与轮次限制、异常响应、并发与断连、会话恢复和锁、工作目录回退、日志归属、当前会话 ID，以及 plain / verbose / JSON 模式。
- **ui**：真实 PTY 中运行 ncurses 客户端，覆盖中文、多行与长文本、粘贴不自动提交、运行中的草稿、工具卡片展开与滚动、两种鼠标编码及分块输入、窗口缩放、网络流与输入并行、退出时恢复终端模式。

迁移期间还用原 C++ 测试程序与原服务器测试过新 C 客户端，原 PTY 检查通过。最终三个测试组全部由 C 编译器构建。

## 真实模型验收

使用本机 `Qwen3.5-4B-Q8_0.gguf`、llama-server 0.4.0（build 10809，commit `5266f24da`）、Metal、两个独立槽，每槽 32768 tokens。验收临时端口为模型 50437、应用 50438；服务在验收后关闭。

`agent-live-check` 实测通过：

- 两个客户端同时收到流式输出。
- 两个连接分别正确回忆各自的 `ALPHA_314159` 和 `BETA_271828`，历史保持独立。
- 断开 A 后，B 继续收到 197 个文本块；A 对应的连接进程已回收。

模型还从默认 C 模板读取说明、创建 `src/main.c`、调用 `make test` 并运行生成的程序，9 轮后正常结束。三次 Bash 调用退出码均为 0。验收时再次手动运行二进制，正常模式输出 `FZU_C_OK`，`--self-test` 输出 `PASS` 并退出 0。

本机记录位于忽略的 `runtime/`，不作为构建依赖：

- `runtime/live-32420-summary.json`
- `runtime/c-refactor-generation-20261014.jsonl`
- `runtime/c-refactor-sessions-20261014/32404-153441571970000/workspace/`

启动模型和应用后，可用 `make run-live-check PORT=8888` 重跑并发验收。生成任务可用：

```sh
build/agent-client --json --prompt '创建一个简单的 C11 程序：正常运行输出 FZU_C_OK；--self-test 验证实际输出函数返回的字符串，成功时输出 PASS 并退出 0。先读取项目 README.md，写入 src/main.c，然后运行 make test。'
```

真实模型生成结果可能不同；以工具执行记录、退出码和生成源码为依据，`task_done.status=success` 单独不能证明程序符合要求。连续修改与错误修复流程由确定性集成测试覆盖。

## 未实测范围

Linux 的构建、ncurses 宽字符链接和 PTY 行为尚未在 Linux 主机运行验证。代码与 CMake 保留 POSIX/Linux 支持，但本次通过的结果仅来自上述 macOS 环境。既有会话中的用户项目和历史文件未作语言迁移。
