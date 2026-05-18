# Perft.cpp - 走法生成性能测试 / Perft Testing

## 中文

### 概述
`Perft.cpp` 实现了 MagnusChess 引擎的 **Perft（走法生成验证）** 系统。Perft 通过计算给定深度下所有可能走法序列的总数来验证走法生成器和走法执行/撤销的正确性，同时也作为一种性能基准测试工具。

### 什么是 Perft？

Perft（Performance Test）将走法生成树展开到指定深度，统计所有可能的走法序列数量：
- `perft(0)` = 1（只有当前局面）
- `perft(1)` = 合法走法数（例如初始局面对应为 20）
- `perft(2)` = 两层的走法序列总数（初局为 400）

通过与已知正确值比较，可以验证走法生成、合法性判断和走法执行/撤销的正确性。

### 核心算法

#### 单线程 Perft（`perft_serial()`）

使用 `do_move_copy()` 创建副本执行走法（不修改原棋盘），递归展开整棵树：
```
for each legal move in position:
    pos_copy = do_move_copy(pos, move)
    count += perft_serial(pos_copy, depth - 1, ...)
```

#### 多线程 Perft（`perft_mt()`）

采用**前沿扩展（Frontier Expansion）** 策略：
1. 主线程将 root 的第一层走法（前沿）分布到工作队列
2. 工作线程从队列取出子树，独立计算各自的部分计数
3. 汇总所有工作线程的计数

**工作窃取（Work Stealing）**：
- 工作线程将队列中的大任务分割为更小的子任务
- 已完成任务的空闲线程从其他线程"窃取"排队任务

#### Divide 命令（`divide()`）

输出第一层走法的 perft 统计：
```
move1: 1234567
move2: 2345678
move3: 3456789
```
支持实时输出（`live_table=true`）和离线输出（每层完成后输出）。

### 标准 Perft 结果（初始局面）

| 深度 | 节点数 |
|------|--------|
| 0 | 1 |
| 1 | 20 |
| 2 | 400 |
| 3 | 8,902 |
| 4 | 197,281 |
| 5 | 4,865,609 |
| 6 | 119,060,324 |
| 7 | 3,195,901,860 |

### 关键函数

| 函数 | 描述 |
|------|------|
| `perft_serial()` | 单线程递归 Perft 实现 |
| `perft_mt()` | 多线程 Perft 实现（前沿扩展） |
| `divide()` | 分行输出 perft 统计 |
| `run_perft()` | Perft 入口，解析参数并选择算法 |

### 设计要点
- 使用 `do_move_copy()` 确保副本不走样，避免 `make/unmake` 的状态管理开销
- 工作窃取策略在多核系统上实现接近线性的加速
- Divide 表格的实时/离线模式提供灵活的可视化选项
- 标准结果硬编码可用于自动回归测试

---

## English

### Overview
`Perft.cpp` implements the **Perft (move generation verification)** system for the MagnusChess engine. Perft validates the correctness of the move generator and move execution/undo by counting all possible move sequences at a given depth, while also serving as a performance benchmarking tool.

### What is Perft?

Perft (Performance Test) expands the move generation tree to a specified depth and counts all possible move sequences:
- `perft(0)` = 1 (just the current position)
- `perft(1)` = number of legal moves (e.g., 20 for the starting position)
- `perft(2)` = total move sequences at depth 2 (400 for starting position)

Comparing against known correct values verifies the correctness of move generation, legality checking, and move execution/undo.

### Core Algorithms

#### Single-Threaded Perft (`perft_serial()`)

Uses `do_move_copy()` to create copies for each move (without modifying the original board), recursively expanding the entire tree:
```
for each legal move in position:
    pos_copy = do_move_copy(pos, move)
    count += perft_serial(pos_copy, depth - 1, ...)
```

#### Multi-Threaded Perft (`perft_mt()`)

Uses a **Frontier Expansion** strategy:
1. Main thread distributes first-layer moves (frontier) into a work queue
2. Worker threads take subtrees from the queue, independently computing their partial counts
3. All worker thread counts are aggregated

**Work Stealing**:
- Worker threads split large queue tasks into smaller subtasks
- Idle threads that finish their work "steal" queued tasks from other threads

#### Divide Command (`divide()`)

Outputs perft statistics for first-layer moves:
```
move1: 1234567
move2: 2345678
move3: 3456789
```
Supports live output (`live_table=true`) and offline output (output after each layer completes).

### Standard Perft Results (Starting Position)

| Depth | Nodes |
|-------|-------|
| 0 | 1 |
| 1 | 20 |
| 2 | 400 |
| 3 | 8,902 |
| 4 | 197,281 |
| 5 | 4,865,609 |
| 6 | 119,060,324 |
| 7 | 3,195,901,860 |

### Key Functions

| Function | Description |
|----------|-------------|
| `perft_serial()` | Single-threaded recursive Perft implementation |
| `perft_mt()` | Multi-threaded Perft implementation (frontier expansion) |
| `divide()` | Per-line perft statistics output |
| `run_perft()` | Perft entry point, parses arguments and selects algorithm |

### Design Notes
- Uses `do_move_copy()` to ensure pristine copies, avoiding the state management overhead of `make/unmake`
- Work stealing strategy achieves near-linear speedup on multi-core systems
- Divide table's live/offline modes provide flexible visualization options
- Standard results are hardcoded for automated regression testing
