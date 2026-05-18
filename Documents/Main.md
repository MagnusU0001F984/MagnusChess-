# Main.cpp - 程序入口 / Program Entry Point

## 中文

### 概述
`Main.cpp` 是 MagnusChess 国际象棋引擎的入口文件。它负责解析命令行参数，然后根据子命令将控制流分派到对应的子系统。

### 核心功能

**`main()`** 函数是整个引擎的启动点。它调用静态函数 `run()`。

**`run()`** 函数执行以下逻辑：
1. 检查命令行参数数量
2. 如果 argc == 1（无子命令），调用 `run_uci()` 进入标准 UCI 协议模式
3. 如果提供了子命令：
   - `"uci"` — 调用 `run_uci()`
   - `"bench"` — 解析可选的深度/时间参数，调用 `run_bench()`
   - `"perft"` — 调用 `run_perft()` 执行性能测试
   - 其他 — 输出帮助信息

### 设计要点
- 设计极简，只做命令路由，所有具体逻辑委托给对应模块
- 支持三种运行模式：UCI 通信模式、搜索基准测试、走法生成测试

---

## English

### Overview
`Main.cpp` is the entry point of the MagnusChess chess engine. It parses command-line arguments and dispatches control flow to the corresponding subsystem based on the subcommand.

### Core Functionality

**`main()`** is the engine's launch point. It calls the static function `run()`.

**`run()`** performs the following logic:
1. Checks the number of command-line arguments
2. If argc == 1 (no subcommand), calls `run_uci()` to enter standard UCI protocol mode
3. If a subcommand is provided:
   - `"uci"` — calls `run_uci()`
   - `"bench"` — parses optional depth/time parameters and calls `run_bench()`
   - `"perft"` — calls `run_perft()` for performance testing
   - Otherwise — outputs help information

### Design Notes
- Extremely minimal design, only performs command routing; all concrete logic is delegated to corresponding modules
- Supports three runtime modes: UCI communication mode, search benchmark, and move generation testing
