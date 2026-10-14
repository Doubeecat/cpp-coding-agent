# Dependencies

- yyjson **0.13.0**, ANSI C, MIT license. Vendored from https://github.com/ibireme/yyjson/tree/0.13.0; source checksums are in `yyjson/SHA256SUMS`, license in `yyjson/LICENSE`.
- libcurl and ncurses are supplied by the system. Normal application builds do not download dependencies.
- llama-server is an independent external process and is not linked into the C application.
