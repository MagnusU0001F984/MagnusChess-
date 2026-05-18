# Uci.cpp - UCI 协议前端 / UCI Protocol Frontend

## 中文

### 概述
`Uci.cpp` 实现了 MagnusChess 引擎的 **UCI（通用国际象棋接口）** 协议前端。它是引擎与外界的通信桥梁，负责解析 GUI 命令、管理搜索会话、处理选项配置以及格式化输出。

### 核心架构

#### UciSession 类

`UciSession` 是 UCI 实现的中心类，封装了完整的协议生命周期：

- **初始化** `init()` — 分配内存、加载表、初始化评估
- **命令循环** `loop()` — 从 `stdin` 逐行读取命令
- **命令派发** `dispatch()` — 将命令路由到对应处理器
- **搜索启动** `go()` — 解析 UCI `go` 参数并启动搜索线程
- **管理选项** `setoption()` — 处理引擎参数配置

**`PvTrackingStreamBuf`** — 自定义 `streambuf` 适配器，拦截搜索线程的输出以提取 PV 信息。

### UCI 协议命令

#### 标准命令

| 命令 | 处理方式 | 描述 |
|------|----------|------|
| `uci` | `handle_uci()` | 输出引擎标识和选项列表 |
| `isready` | `handle_isready()` | 输出 `readyok` |
| `setoption name <n> value <v>` | `handle_setoption()` | 设置引擎参数 |
| `position [fen <f> \| startpos] moves <m>...` | `handle_position()` | 设置棋盘局面 |
| `go <params>` | `handle_go()` | 启动搜索 |

#### 自定义命令

| 命令 | 描述 |
|------|------|
| `bench [depth] [time]` | 运行搜索基准测试 |
| `perft [depth]` | 运行走法生成性能测试 |
| `eval` | 输出当前局面评估 |
| `quit` | 退出引擎 |

#### `go` 命令参数

| 参数 | 描述 |
|------|------|
| `wtime/btime` | 白方/黑方剩余时间（毫秒） |
| `winc/binc` | 白方/黑方每步增量（毫秒） |
| `movestogo` | 到时间控制的剩余步数 |
| `depth` | 固定搜索深度 |
| `nodes` | 固定搜索节点数 |
| `movetime` | 固定搜索时间 |
| `infinite` | 无限搜索 |
| `ponder` | 长考模式 |
| `mate` | 搜索直到找到某步杀 |

### 搜索线程管理

- `UciSession::search_thread()` — 在独立线程中运行 `iterative_deepening()`
- 通过 `std::atomic<bool>` 标志实现线程安全终止
- `PvTrackingStreamBuf` 拦截 `std::cout` 输出，解析 `info string pv ...` 行

### 配置选项（UCI Options）

| 选项 | 类型 | 描述 |
|------|------|------|
| `Hash` | Spin (0-65536) | 置换表大小（MB） |
| `Threads` | Spin (1-512) | 搜索线程数 |
| `MoveOverhead` | Spin (0-10000) | 走法开销补偿（ms） |
| `ClearHash` | Button | 清除置换表 |
| `EvalFile` | String | NNUE 网络文件路径 |

### 输出格式

**`info` 输出**：
- `info depth <d> seldepth <s> multipv <n> score cp <x> nodes <n> nps <n> time <t> pv <moves>`
- `info string <message>` — 调试/日志消息

**`bestmove` 输出**：
- `bestmove <move> [ponder <move>]`

### 设计要点
- 自定义 `streambuf` 注入实现 PV 跟踪，避免修改搜索库的内部代码
- 长考模式使用独立的时间偏移量 (`ponder_offset_tp`)
- 线程安全的 `dispatch` 使用原子标志协调搜索线程
- FEN 解析支持标准格式

---

## English

### Overview
`Uci.cpp` implements the **UCI (Universal Chess Interface)** protocol frontend for the MagnusChess engine. It is the communication bridge between the engine and the outside world, responsible for parsing GUI commands, managing search sessions, handling option configuration, and formatting output.

### Core Architecture

#### UciSession Class

`UciSession` is the central class of the UCI implementation, encapsulating the complete protocol lifecycle:

- **Initialization** `init()` — Allocates memory, loads tables, initializes evaluation
- **Command loop** `loop()` — Reads commands line by line from `stdin`
- **Command dispatch** `dispatch()` — Routes commands to corresponding handlers
- **Search launch** `go()` — Parses UCI `go` parameters and starts the search thread
- **Option management** `setoption()` — Handles engine parameter configuration

**`PvTrackingStreamBuf`** — Custom `streambuf` adapter that intercepts search thread output to extract PV information.

### UCI Protocol Commands

#### Standard Commands

| Command | Handler | Description |
|---------|---------|-------------|
| `uci` | `handle_uci()` | Output engine identity and options list |
| `isready` | `handle_isready()` | Output `readyok` |
| `setoption name <n> value <v>` | `handle_setoption()` | Set engine parameters |
| `position [fen <f> \| startpos] moves <m>...` | `handle_position()` | Set up board position |
| `go <params>` | `handle_go()` | Start search |

#### Custom Commands

| Command | Description |
|---------|-------------|
| `bench [depth] [time]` | Run search benchmark |
| `perft [depth]` | Run move generation performance test |
| `eval` | Output current position evaluation |
| `quit` | Exit engine |

#### `go` Command Parameters

| Parameter | Description |
|-----------|-------------|
| `wtime/btime` | White/Black remaining time (ms) |
| `winc/binc` | White/Black increment per move (ms) |
| `movestogo` | Moves remaining until time control |
| `depth` | Fixed search depth |
| `nodes` | Fixed search node count |
| `movetime` | Fixed search time |
| `infinite` | Infinite search |
| `ponder` | Ponder mode |
| `mate` | Search until mate in N found |

### Search Thread Management

- `UciSession::search_thread()` — Runs `iterative_deepening()` in a separate thread
- Thread-safe termination via `std::atomic<bool>` flags
- `PvTrackingStreamBuf` intercepts `std::cout` output, parsing `info string pv ...` lines

### Configuration Options (UCI Options)

| Option | Type | Description |
|--------|------|-------------|
| `Hash` | Spin (0-65536) | Transposition table size (MB) |
| `Threads` | Spin (1-512) | Number of search threads |
| `MoveOverhead` | Spin (0-10000) | Move overhead compensation (ms) |
| `ClearHash` | Button | Clear transposition table |
| `EvalFile` | String | NNUE network file path |

### Output Format

**`info` output**:
- `info depth <d> seldepth <s> multipv <n> score cp <x> nodes <n> nps <n> time <t> pv <moves>`
- `info string <message>` — Debug/log messages

**`bestmove` output**:
- `bestmove <move> [ponder <move>]`

### Design Notes
- Custom `streambuf` injection enables PV tracking without modifying search library internals
- Ponder mode uses an independent time offset (`ponder_offset_tp`)
- Thread-safe dispatch using atomic flags to coordinate with the search thread
- FEN parsing supports standard format
