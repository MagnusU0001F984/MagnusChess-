# Search.cpp - 搜索引擎 / Search Engine

## 中文

### 概述
`Search.cpp` 是 MagnusChess 引擎的核心——完整的**搜索引擎**实现（约 135KB）。它实现了主变例搜索（PVS）、Alpha-Beta 剪枝，以及数十种现代搜索优化技术。

### 核心架构

#### 搜索框架

**`iterative_deepening()`** — 迭代加深框架：
```
for depth in 1..=max_depth:
    aspiration_window = [prev_score - delta, prev_score + delta]
    do:
        score = pvs(alpha, beta, depth, ply=0)
        if fail-low: widen window downward, re-search
        if fail-high: widen window upward, re-search
    while window fails
    输出 info 和 bestmove
```

**`pvs()`** — 主变例搜索（Principal Variation Search）：
```
if first_move:
    score = -pvs(-beta, -alpha, depth-1, ply+1)  // 全窗口
else:
    score = -pvs(-alpha-1, -alpha, depth-1, ply+1)  // 零窗口
    if score > alpha:
        score = -pvs(-beta, -alpha, depth-1, ply+1)  // 全窗口重新搜索
```

**`qsearch()`** — 静止搜索（Quiescence Search）：
- 仅搜索吃子走法（解决地平线效应）
- 站定截止（stand-pat）评估
- Delta 剪枝：如果 eval + queen_val < alpha，跳过

#### 辅助结构

**`Searcher`** — 上下文聚合结构体，包含搜索所需的全部数据引用：
- 局面 position
- 搜索栈 ss[]
- 置换表 TT
- 历史表 history
- 时间限制 limits
- 统计计数器 nodes, tb_hits 等

**`SearchStackEntry`** — 每层存储：
- `static_eval` — 静态评估值
- `move` — 当前尝试的走法
- `excluded_move` — 奇异延展排除的走法
- `pv[ply]` — 该层的 PV 线段
- `continuation_history` — 延续历史指针

### 搜索优化技术全景

#### 节点进入时的裁剪

| 技术 | 条件 | 动作 |
|------|------|------|
| **TT 探测** | TT 命中且深度足够 | 返回缓存分数（可能触发 β 剪枝） |
| **Razoring** | `eval + margin < alpha`，深度很浅 | 返回 qsearch 结果 |
| **反向无效裁剪（RFP）** | `eval - margin >= beta` | 跳过搜索，返回 eval |
| **空走裁剪（NMP）** | 见 Nmp.cpp 文档 | 让对方走两步测试 |
| **Probcut** | TT 命中高深度 + 低边界 | 用缩减深度验证捕获 β 剪枝 |
| **小 Probcut** | 类似 Probcut，深度更浅 | 额外的低代价验证 |

#### 走法循环中的裁剪

| 技术 | 条件 | 动作 |
|------|------|------|
| **SEE 门控** | SEE < 阈值 | 将吃子发送到"坏吃子"队列 |
| **吃子无效裁剪** | `eval + capture_gain < alpha` | 跳过该吃子 |
| **坏吃子裁剪** | SEE << 0，深度低 | 停止搜索更多吃子 |
| **历史裁剪** | 安静走法历史得分太低 | 静默跳过该走法 |
| **奇异延展（SE）** | 只有一个走法明显好于其他 | 对该走法延展搜索深度 |
| **迟走法裁剪（LMR）** | 见 Lmr.cpp 文档 | 降低非优先走法搜索深度 |

#### 节点完成时的处理

| 技术 | 描述 |
|------|------|
| **TT 保存** | 将结果存入置换表 |
| **历史更新** | 奖励触发剪枝的走法，惩罚未触发的走法 |
| **杀手更新** | 更新杀手走法槽位 |
| **反走法更新** | 设置反走法关联 |
| **延续历史更新** | 更新 (prev_move, reply) 关联 |

### 搜索核心流程

#### qsearch() 流程

```
qsearch(alpha, beta, ply):
    → 站定评估 stand_pat = resolve_static_eval()
    → if stand_pat >= beta: return stand_pat  (站定剪枝)
    → alpha = max(alpha, stand_pat)

    → TT 探测
    → MovePicker 构建（仅吃子阶段）
    → for each capture:
        → delta 剪枝
        → SEE 裁剪
        → 执行走法
        → score = -qsearch(-beta, -alpha, ply+1)
        → 撤销走法
        → alpha = max(alpha, score)
        → if alpha >= beta: break
    → return alpha
```

#### pvs() 流程

```
pvs(alpha, beta, depth, ply, cut_node):
    → 重复/50步 检测
    → 将杀距离裁剪
    → TT 探测（可能直接返回或缩小窗口）

    → resolve_static_eval()  // 解析或回退到增量评估
    → improving = 如果 eval 相比两回合前改善

    → Razoring (depth ≤ threshold)
    → RFP (反向无效裁剪)
    → NMP (空走裁剪)
    → Probcut

    → MovePicker 构建（TT_MOVE → GOOD_CAPTURES → KILLERS → QUIETS → BAD_CAPTURES）
    → for each move:
        → 深度 ≤ 门槛且走法足够靠后:
            → SEE 坏吃子门控
            → 吃子无效裁剪
            → 坏吃子裁剪

        → 安静走法:
            → 历史裁剪
            → 奇异延展深度减免
            → 奇异延展（SE）

        → LMR 决策 → 缩减窗口搜索
        → if 缩减搜索通过 → 全深度重新搜索
        → α 更新 → if α ≥ β:
            → 历史更新（奖励影响 PV 的走法）
            → TT 保存
            → return α  (β 剪枝)

    → 所有走法都未触发剪枝 → TT 保存 ALL_NODE
    → return alpha
```

### 关键辅助系统

#### 静态评估解析（`resolve_static_eval()`）

优先级：
1. 如果无现存评估 → NNUE 或 HCE 评估
2. 如果局面有将军 → 重新评估（评估值可能因将军动态而过时）
3. 否则 → 使用增量评估值

#### 改进检测（`improving_position()`）

比较本回合与两回合前的静态评估，判断局面是否在改善。用于调整裁剪裕度。

#### 置换表分数转换

| 函数 | 描述 |
|------|------|
| `tt_score_to_mated_in()` | 将 TT 存储的将杀分数转换为搜索中使用的将杀分数 |
| `mated_in_to_tt_score()` | 反向转换 |

#### 矫正历史（Correction History）

一种评估值校正机制，根据历史搜索结果调整静态评估值，使评估更接近实际搜索结果。

### 关键常量

| 常量 | 值 | 描述 |
|------|-----|------|
| `VALUE_INF` | 32000 | 无限大值 |
| `VALUE_MATE` | 31000 | 将杀基础值 |
| `VALUE_KNOWN_WIN` | 10000 | 已知必胜阈值 |
| `MAX_PLY` | 128 | 最大搜索层数 |

### 设计要点
- 所有裁剪裕度都随深度和改善状态动态调整
- TT 切割节点概率（cutNode）标志优化零窗口搜索
- LMR 使用 FP 精度实现连续裁剪量调整
- 历史更新同时影响安静走法和吃子走法的排序
- 奇异延展提供战术发现能力
- Probcut 在较高层做浅搜索提前发现 β 剪枝机会

---

## English

### Overview
`Search.cpp` is the core of the MagnusChess engine — the complete **search engine** implementation (~135KB). It implements Principal Variation Search (PVS) with Alpha-Beta pruning and dozens of modern search optimization techniques.

### Core Architecture

#### Search Framework

**`iterative_deepening()`** — Iterative deepening framework:
```
for depth in 1..=max_depth:
    aspiration_window = [prev_score - delta, prev_score + delta]
    do:
        score = pvs(alpha, beta, depth, ply=0)
        if fail-low: widen window downward, re-search
        if fail-high: widen window upward, re-search
    while window fails
    output info and bestmove
```

**`pvs()`** — Principal Variation Search:
```
if first_move:
    score = -pvs(-beta, -alpha, depth-1, ply+1)  // full window
else:
    score = -pvs(-alpha-1, -alpha, depth-1, ply+1)  // null window
    if score > alpha:
        score = -pvs(-beta, -alpha, depth-1, ply+1)  // full window re-search
```

**`qsearch()`** — Quiescence Search:
- Searches only capture moves (solves the horizon effect)
- Stand-pat cutoff evaluation
- Delta pruning: if eval + queen_val < alpha, skip

#### Helper Structures

**`Searcher`** — Context aggregation struct containing all data references needed for search:
- Position
- Search stack ss[]
- Transposition table TT
- History tables history
- Time limits limits
- Statistics counters nodes, tb_hits, etc.

**`SearchStackEntry`** — Per-ply storage:
- `static_eval` — Static evaluation value
- `move` — Move currently being tried
- `excluded_move` — Move excluded for singular extension
- `pv[ply]` — PV segment for this ply
- `continuation_history` — Continuation history pointer

### Search Optimization Techniques Overview

#### Node Entry Pruning

| Technique | Condition | Action |
|-----------|-----------|--------|
| **TT Probe** | TT hit with sufficient depth | Return cached score (may trigger β cutoff) |
| **Razoring** | `eval + margin < alpha`, very shallow depth | Return qsearch result |
| **Reverse Futility (RFP)** | `eval - margin >= beta` | Skip search, return eval |
| **Null Move Pruning (NMP)** | See Nmp.cpp docs | Let opponent move twice test |
| **Probcut** | TT hit at high depth + lower bound | Verify with reduced-depth search for β cutoff |
| **Small Probcut** | Like Probcut, shallower depth | Additional low-cost verification |

#### In-Move-Loop Pruning

| Technique | Condition | Action |
|-----------|-----------|--------|
| **SEE Gating** | SEE < threshold | Send capture to "bad captures" queue |
| **Capture Futility** | `eval + capture_gain < alpha` | Skip this capture |
| **Bad Capture Pruning** | SEE << 0, low depth | Stop searching more captures |
| **History Pruning** | Quiet move history score too low | Silently skip this move |
| **Singular Extension (SE)** | Only one move significantly better than others | Extend search depth for that move |
| **Late Move Reduction (LMR)** | See Lmr.cpp docs | Reduce search depth for non-priority moves |

#### Node Completion

| Technique | Description |
|-----------|-------------|
| **TT Save** | Store result in transposition table |
| **History Update** | Reward cutoff-causing moves, penalize non-cutoffs |
| **Killer Update** | Update killer move slots |
| **Countermove Update** | Set countermove association |
| **Continuation History Update** | Update (prev_move, reply) associations |

### Core Search Flow

#### qsearch() Flow

```
qsearch(alpha, beta, ply):
    → stand_pat = resolve_static_eval()
    → if stand_pat >= beta: return stand_pat  (stand-pat cutoff)
    → alpha = max(alpha, stand_pat)

    → TT probe
    → MovePicker construction (capture stages only)
    → for each capture:
        → delta pruning
        → SEE pruning
        → make move
        → score = -qsearch(-beta, -alpha, ply+1)
        → unmake move
        → alpha = max(alpha, score)
        → if alpha >= beta: break
    → return alpha
```

#### pvs() Flow

```
pvs(alpha, beta, depth, ply, cut_node):
    → Repetition / 50-move detection
    → Mate distance pruning
    → TT probe (may return immediately or narrow window)

    → resolve_static_eval()  // Resolve or fallback to incremental eval
    → improving = if eval improved vs. two plies ago

    → Razoring (depth ≤ threshold)
    → RFP (Reverse Futility Pruning)
    → NMP (Null Move Pruning)
    → Probcut

    → MovePicker construction (TT_MOVE → GOOD_CAPTURES → KILLERS → QUIETS → BAD_CAPTURES)
    → for each move:
        → depth ≤ threshold && move late enough:
            → SEE bad capture gating
            → capture futility pruning
            → bad capture pruning

        → Quiet moves:
            → history pruning
            → singular extension depth reduction
            → singular extension (SE)

        → LMR decision → reduced window search
        → if reduced search passes → full-depth re-search
        → α update → if α ≥ β:
            → history update (reward moves that affect PV)
            → TT save
            → return α  (β cutoff)

    → All moves failed → TT save ALL_NODE
    → return alpha
```

### Key Auxiliary Systems

#### Static Evaluation Resolution (`resolve_static_eval()`)

Priority:
1. If no existing evaluation → NNUE or HCE evaluation
2. If position is in check → re-evaluate (evaluation may be stale due to check dynamics)
3. Otherwise → use incremental evaluation value

#### Position Improvement Detection (`improving_position()`)

Compares static evaluation between this ply and two plies ago to determine if the position is improving, used to adjust pruning margins.

#### TT Score Conversion

| Function | Description |
|----------|-------------|
| `tt_score_to_mated_in()` | Convert TT-stored mate score to search-usable mate score |
| `mated_in_to_tt_score()` | Reverse conversion |

#### Correction History

An evaluation correction mechanism that adjusts static evaluation values based on historical search results, making the evaluation closer to actual search outcomes.

### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `VALUE_INF` | 32000 | Infinite value |
| `VALUE_MATE` | 31000 | Mate base value |
| `VALUE_KNOWN_WIN` | 10000 | Known win threshold |
| `MAX_PLY` | 128 | Maximum search depth |

### Design Notes
- All pruning margins dynamically adjust with depth and improving state
- TT cutNode flag optimizes null-window searches
- LMR uses fractional ply (FP) precision for continuous reduction adjustment
- History updates affect both quiet and capture move ordering
- Singular extensions provide tactical discovery capability
- Probcut does shallow searches at higher levels to discover β cutoff opportunities early
