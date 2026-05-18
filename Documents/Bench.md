# Bench.cpp - 基准测试 / Benchmark

## 中文

### 概述
`Bench.cpp` 实现了 MagnusChess 引擎的**基准测试（Benchmark）** 系统。通过在一组预定义的国际象棋局面上运行固定深度或固定时间的搜索，量化引擎的搜索速度和性能。

### 核心架构

#### 测试局面集

基准测试使用 **13 个标准 FEN 局面**，涵盖各种国际象棋阶段和特征：
- 开局、中局、残局
- 封闭/开放局面
- 对称/不平衡子力
- 包含将杀威胁和战术机会的局面

这些局面来自 Stockfish 的 benchmark 集，确保同类引擎间的可比性。

#### 基准测试模式

| 模式 | 函数 | 描述 |
|------|------|------|
| 固定深度 | `run_search_bench()` | 对每个局面搜索固定深度，统计节点和时间 |
| 固定时间 | `run_timed_search_bench()` | 对每个局面搜索固定时间 |
| Perft | `benchmark_perft()` | 走法生成性能测试 |

#### 配置解析（`parse_config()`）

从 CLI 参数解析基准测试参数：
```
magnus bench [depth <d>] [time <ms>] [threads <n>] [hash <mb>]
```

### 搜索基准测试流程（`run_search_bench()`）

```
对每个测试局面:
    1. set_start_position(FEN)
    2. 初始化搜索限制
    3. 运行 iterative_deepening()
    4. 收集 <nodes, time, nps>
总计:
    输出: "Total: <total_nodes> nodes <total_time> ms <total_nps> nps"
```

### 关键函数

| 函数 | 描述 |
|------|------|
| `run_search_bench()` | 固定深度搜索基准测试 |
| `run_timed_search_bench()` | 固定时间搜索基准测试 |
| `benchmark_perft()` | Perft 性能测试包装器 |
| `parse_config()` | 解析 CLI 参数 |
| `set_start_position()` | 设置基准测试局面 |

### 输出格式

```
Position 1/13: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
Nodes: 1234567, Time: 1234 ms, NPS: 1000456
...
Total: 16000000 nodes 16000 ms 1000000 nps
```

### 设计要点
- 13 个局面覆盖完整的棋局多样性
- 支持单线程和多线程模式
- 与 UCI 前端集成，可通过 `bench` 命令交互调用
- NPS 计算为速度和扩展性的客观度量

---

## English

### Overview
`Bench.cpp` implements the **benchmark** system for the MagnusChess engine. By running fixed-depth or fixed-time searches on a set of predefined chess positions, it quantifies the engine's search speed and performance.

### Core Architecture

#### Test Position Set

The benchmark uses **13 standard FEN positions** covering various chess phases and characteristics:
- Openings, middlegames, endgames
- Closed/open positions
- Symmetric/imbalanced material
- Positions with mating threats and tactical opportunities

These positions come from Stockfish's benchmark set, ensuring comparability between similar engines.

#### Benchmark Modes

| Mode | Function | Description |
|------|----------|-------------|
| Fixed depth | `run_search_bench()` | Search each position to a fixed depth, tracking nodes and time |
| Fixed time | `run_timed_search_bench()` | Search each position for a fixed duration |
| Perft | `benchmark_perft()` | Move generation performance testing |

#### Configuration Parsing (`parse_config()`)

Parses benchmark parameters from CLI arguments:
```
magnus bench [depth <d>] [time <ms>] [threads <n>] [hash <mb>]
```

### Search Benchmark Flow (`run_search_bench()`)

```
For each test position:
    1. set_start_position(FEN)
    2. Initialize search limits
    3. Run iterative_deepening()
    4. Collect <nodes, time, nps>
Total:
    Output: "Total: <total_nodes> nodes <total_time> ms <total_nps> nps"
```

### Key Functions

| Function | Description |
|----------|-------------|
| `run_search_bench()` | Fixed-depth search benchmark |
| `run_timed_search_bench()` | Fixed-time search benchmark |
| `benchmark_perft()` | Perft performance test wrapper |
| `parse_config()` | Parse CLI arguments |
| `set_start_position()` | Set up benchmark position |

### Output Format

```
Position 1/13: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
Nodes: 1234567, Time: 1234 ms, NPS: 1000456
...
Total: 16000000 nodes 16000 ms 1000000 nps
```

### Design Notes
- 13 positions cover complete chess diversity
- Supports both single-threaded and multi-threaded modes
- Integrated with UCI frontend, invocable interactively via `bench` command
- NPS calculation serves as an objective measure of speed and scalability
