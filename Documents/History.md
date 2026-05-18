# History.cpp - 历史启发式表 / History Heuristic Tables

## 中文

### 概述
`History.cpp` 实现了 MagnusChess 搜索引擎中所有历史启发式表格的辅助函数，包括深度分类、SEE 分类、历史奖励/惩罚计算以及 `HistoryTables` 的清理。

### 核心概念

#### 深度分类（DepthClass）
将搜索深度分为四个类别，用于在不同深度区间应用不同的历史偏置：
```
Shallow   (d ≤ 3)：浅层节点
MediumLow (3 < d ≤ 6)：中低深度
MediumHigh(6 < d ≤ 10)：中高深度
Deep      (d > 10)：深层节点
```

#### SEE 分类（SeeClass）
将 SEE 值分为七类，用于历史启发式中的走法质量判断：

| SeeClass | 条件 | 含义 |
|----------|------|------|
| `LossBig` | SEE ≤ -100 | 大亏 |
| `LossSmall` | -100 < SEE < 0 | 小亏 |
| `Equal` | SEE == 0 | 等价交换 |
| `GainSmall` | 0 < SEE < 200 | 小赚 |
| `GainBig` | SEE ≥ 200 | 大赚 |
| `Promo` | 升变 | 升变吃子 |
| `Check` | 将军 | 带将吃子 |

**`classify_see_bias()`** — 简化版的四类分类，用于 SEE 偏置表：
```
LossSmall (SEE < -50)
Equal     (-50 ≤ SEE ≤ 50)
GainSmall (50 < SEE ≤ 200)
GainBig  (SEE > 200)
```

#### 历史奖励/惩罚函数

**`history_bonus(depth)`** — 走法因触发剪枝而获得的奖励：
```
bonus = depth × depth (最小为 1)
```

**`history_penalty(depth)`** — 走法因未触发剪枝而受到的惩罚：
```
penalty = bonus × 4
```
惩罚力度是奖励的 4 倍，体现引擎对坏走法的"记忆"比对好走法的强。

#### SEE 即时项（`see_immediate_term()`）
将 SEE 原始值按预设方案缩放到一个范围，作为走法排序中的即时 SEE 项：

| 方案 | 公式 | 范围 |
|------|------|------|
| Weak | `clamp(see/4, -75, 75)` | [-75, 75] |
| Medium | `clamp(see/2, -150, 150)` | [-150, 150] |
| Strong | `clamp(see, -300, 300)` | [-300, 300] |

#### HistoryTables::clear()
将所有历史表清零，包括：杀手走法、安静走法历史、吃子历史、反走法、延续历史、SEE 偏置和兵形历史。

---

## English

### Overview
`History.cpp` implements auxiliary functions for all history heuristic tables in the MagnusChess search engine, including depth classification, SEE classification, history bonus/penalty computation, and `HistoryTables` cleanup.

### Core Concepts

#### Depth Classification (DepthClass)
Divides search depth into four categories for applying different history biases at different depth ranges:
```
Shallow   (d ≤ 3): Shallow nodes
MediumLow (3 < d ≤ 6): Medium-low depth
MediumHigh(6 < d ≤ 10): Medium-high depth
Deep      (d > 10): Deep nodes
```

#### SEE Classification (SeeClass)
Divides SEE values into seven categories for move quality judgment in history heuristics:

| SeeClass | Condition | Meaning |
|----------|-----------|---------|
| `LossBig` | SEE ≤ -100 | Large loss |
| `LossSmall` | -100 < SEE < 0 | Small loss |
| `Equal` | SEE == 0 | Equal exchange |
| `GainSmall` | 0 < SEE < 200 | Small gain |
| `GainBig` | SEE ≥ 200 | Large gain |
| `Promo` | Promotion | Promotion capture |
| `Check` | Check | Capture with check |

**`classify_see_bias()`** — Simplified four-category classification for the SEE bias table:
```
LossSmall (SEE < -50)
Equal     (-50 ≤ SEE ≤ 50)
GainSmall (50 < SEE ≤ 200)
GainBig  (SEE > 200)
```

#### History Bonus/Penalty Functions

**`history_bonus(depth)`** — Reward for a move that caused a cutoff:
```
bonus = depth × depth (minimum 1)
```

**`history_penalty(depth)`** — Penalty for a move that failed to cause a cutoff:
```
penalty = bonus × 4
```
The penalty is 4× the bonus, reflecting that the engine "remembers" bad moves more strongly than good ones.

#### SEE Immediate Term (`see_immediate_term()`)
Scales the raw SEE value into a bounded range according to a preset, used as an immediate SEE term in move ordering:

| Preset | Formula | Range |
|--------|---------|-------|
| Weak | `clamp(see/4, -75, 75)` | [-75, 75] |
| Medium | `clamp(see/2, -150, 150)` | [-150, 150] |
| Strong | `clamp(see, -300, 300)` | [-300, 300] |

#### HistoryTables::clear()
Zeroes all history tables: killer moves, quiet history, capture history, countermoves, continuation history, SEE bias, and pawn history.
