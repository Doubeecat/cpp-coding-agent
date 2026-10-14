# 工作汇总

2026-10-14

## 已完成

- 将程序、测试、默认模板和排序演示从 C++ 迁移到 C11，保留外部 llama-server。
- 移除旧 `.cpp` / `.hpp`、C++ 模板及 nlohmann/json，改用固定版本的 yyjson；原 C++ 实现保存在 `main` 和 Git 历史中。
- 按 JSON、协议、工作目录、工具、模型、Agent 和界面拆分模块，明确错误处理与资源释放。
- 保留五工具、流式输出、上下文预算、超时取消、中文 TUI、粘贴和鼠标操作。
- 修复会话恢复：`--resume` 和 `/resume` 显示旧对话及工具卡片，长历史分块传输；完善锁、日志归属和命令编号。
- 新建 README 和验收文档，更新构建入口及架构说明。

## 验证

- 核心、集成、真实 PTY 测试：3/3 通过。
- ASan/UBSan：3/3 通过；不需要 C++ 编译器即可构建。
- 真实模型的 C 程序生成、自测、并发、会话隔离和断连回收已通过；Linux 尚未实机验证。

## Git 状态

- 本地分支：`c-agent`；代码提交：`885cb5a`。
- 目标仓库：[Doubeecat/cpp-coding-agent](https://github.com/Doubeecat/cpp-coding-agent)。
- GitHub 推送尚未完成：本机 Git 连接遇到 `github.com` DNS 解析失败。

使用说明见 [README.md](README.md)，详细验证见 [TESTING.md](TESTING.md)。
