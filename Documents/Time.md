# Time.cpp - 时间管理 / Time Management

## 中文

### 概述
`Time.cpp` 实现了 MagnusChess 引擎的**时间管理**系统。它解析 UCI `go` 命令的时间参数，使用 Stockfish 风格的动态时间分配公式，为每次搜索计算最优的软/硬时间限制。

### 核心架构

#### 搜索限制结构（`SearchLimits`）

```cpp
struct SearchLimits {
    int64_t time[COLOR_NB];     // 各方剩余时间 (ms)
    int64_t inc[COLOR_NB];      // 各方增量 (ms)
    int movestogo;              // 到时间控制的剩余步数
    int depth;                  // 固定深度（0 = 无限制）
    int64_t movetime;           // 固定搜索时间
    int64_t nodes;              // 固定节点数
    int mate;                   // 杀棋搜索深度
    bool infinite;              // 无限模式
    bool ponder;                // 长考模式

    // 计算所得的限制
    TimePoint soft_limit;       // 软时间限制
    TimePoint hard_limit;       // 硬时间限制
};
```

#### 时间分配流程（`build_limits()`）

```
1. 解析 UCI go 参数
2. 如果指定了 movetime/depth/nodes → 固定限制
3. 否则：
   a. 计算可用时间 = time[side] + inc[side] - move_overhead
   b. 根据 movestogo 或默认预估步数分摊时间
   c. 软限制 = 可用时间 × scale_factor（通常 0.5-0.75）
   d. 硬限制 = 可用时间 × max_factor（通常 1.5-2.5）
```

#### Stockfire 风格分配公式

**剩余步数估计**：
- 如果指定了 `movestogo`，直接使用
- 否则，根据历史平均步数和时间控制估算

**基本分配**：
```
opt_time = (剩留时间) / (剩余步数) + 增量
soft_limit = opt_time × soft_factor
hard_limit = opt_time × hard_factor
max_time = min(opt_time × max_factor, 剩留时间)
```

#### 历史追踪（`record_search()`）

跟踪最近的搜索时间使用情况，用于自适应调整：
- 记录每次搜索的实际用时
- 根据历史模式预测当前搜索的合理时间
- 当引擎频繁超时时调整分配比例

#### 紧急时间扩展

在以下情况扩展时间预算：
- 最佳走法发生变化（PV 变化）
- 分数在下降
- 时间充裕且局面尚不明朗

### 关键函数

| 函数 | 描述 |
|------|------|
| `build_limits()` | 从 UCI 参数构建搜索限制 |
| `record_search()` | 记录搜索历史以调整未来分配 |
| `elapsed()` | 返回搜索已过的时间 |
| `should_stop()` | 基于限制判断是否应停止搜索 |

### 设计要点
- 动态分配策略避免了固定时间均分的粗暴方式
- 历史追踪实现了经验驱动的自适应时间管理
- 软限制用于"考虑停止"，硬限制用于强制停止
- 移动开销补偿（MoveOverhead）防止通信延迟导致的超时

---

## English

### Overview
`Time.cpp` implements the **time management** system for the MagnusChess engine. It parses time parameters from UCI `go` commands, uses a Stockfish-style dynamic time allocation formula, and computes optimal soft/hard time limits for each search.

### Core Architecture

#### Search Limits Structure (`SearchLimits`)

```cpp
struct SearchLimits {
    int64_t time[COLOR_NB];     // Remaining time per side (ms)
    int64_t inc[COLOR_NB];      // Increment per side (ms)
    int movestogo;              // Moves remaining to time control
    int depth;                  // Fixed depth (0 = no limit)
    int64_t movetime;           // Fixed search time
    int64_t nodes;              // Fixed node count
    int mate;                   // Mate search depth
    bool infinite;              // Infinite mode
    bool ponder;                // Ponder mode

    // Computed limits
    TimePoint soft_limit;       // Soft time limit
    TimePoint hard_limit;       // Hard time limit
};
```

#### Time Allocation Flow (`build_limits()`)

```
1. Parse UCI go parameters
2. If movetime/depth/nodes specified → fixed limits
3. Otherwise:
   a. Compute available time = time[side] + inc[side] - move_overhead
   b. Amortize time based on movestogo or default estimated move count
   c. Soft limit = available × scale_factor (typically 0.5-0.75)
   d. Hard limit = available × max_factor (typically 1.5-2.5)
```

#### Stockfish-Style Allocation Formula

**Remaining moves estimation**:
- If `movestogo` is specified, use it directly
- Otherwise, estimate based on historical average moves and time control

**Basic allocation**:
```
opt_time = (remaining_time) / (moves_remaining) + increment
soft_limit = opt_time × soft_factor
hard_limit = opt_time × hard_factor
max_time = min(opt_time × max_factor, remaining_time)
```

#### History Tracking (`record_search()`)

Tracks recent search time usage for adaptive adjustment:
- Records actual time used per search
- Predicts reasonable time for current search based on historical patterns
- Adjusts allocation ratios when the engine frequently overruns time

#### Emergency Time Extension

Extends the time budget when:
- Best move changes (PV change)
- Score is dropping
- Time is ample and position is unclear

### Key Functions

| Function | Description |
|----------|-------------|
| `build_limits()` | Build search limits from UCI parameters |
| `record_search()` | Record search history for future allocation adjustment |
| `elapsed()` | Return elapsed search time |
| `should_stop()` | Determine whether to stop based on limits |

### Design Notes
- Dynamic allocation avoids the crude approach of equal time division
- History tracking enables empirically-driven adaptive time management
- Soft limit for "consider stopping," hard limit for forced stop
- Move overhead compensation (MoveOverhead) prevents timeouts from communication latency
