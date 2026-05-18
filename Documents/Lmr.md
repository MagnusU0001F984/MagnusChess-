# Lmr.cpp - 迟走法裁剪 / Late Move Reduction

## 中文

### 概述
`Lmr.cpp` 实现了 MagnusChess 引擎的**迟走法裁剪（Late Move Reduction, LMR）** 系统。LMR 是提高搜索效率的核心技术：对排序靠后的走法使用降低的搜索深度，根据走法质量和搜索上下文动态调整减少量。

### 核心架构

#### 分精度 Ply（`FP_ONE_PLY`）

LMR 使用**分数精度 ply**进行计算，`FP_ONE_PLY = 1024` 代表一个标准搜索层。这允许裁剪量不是整层，而是连续可调的：
```
base_depth = 原始深度（以 ply 为单位）
reduction = 计算出的减少量（以 FP 为单位）
search_depth = base_depth - (reduction / FP_ONE_PLY)
```

#### 走法分类

LMR 区分两类走法，采用不同的裁剪策略：

1. **安静走法裁剪** — 使用 `decide_lmr_quiet()` 函数族
2. **吃子走法裁剪** — 使用 `decide_lmr_capture()` 函数族

### 安静走法裁剪逻辑（`decide_lmr_quiet()`）

**基础裁剪公式**（与 Stockfire 类似）：
```
reduction = base + ln(depth_factor) × ln(move_index_offset)
```

**调整因子**（减少裁剪，即给更有希望的走法更小的裁剪）：

| 条件 | 调整 | 原因 |
|------|------|------|
| 是 PV 节点 | 减少裁剪 | PV 走法更值得谨慎对待 |
| 走法给将军 | 减少裁剪 | 将军走法可能强制改变局面 |
| 高历史得分 | 减少裁剪（`lmr_history_max`） | 历史高的走法更可靠 |
| 前一个是杀手走法 | 减少裁剪 | 杀手走法暗示活跃局面 |
| 前一个走法给将军 | 减少裁剪 | 动态局面需要更仔细 |
| 走法改善局面 | 减少裁剪 | improving score 相关调整 |

### 吃子裁剪逻辑（`decide_lmr_capture()`）

对 SEE < 0 的吃子走法应用额外裁剪，裁剪量与 SEE 损失成正比。

### 关键函数

| 函数 | 描述 |
|------|------|
| `decide_lmr_quiet()` | 决定安静走法的 LMR 裁剪量（以 FP 为单位） |
| `decide_lmr_capture()` | 决定吃子走法的 LMR 裁剪量 |
| `lmr_research_depth()` | 计算 LMR 重新搜索时的深度（如果空窗失败） |
| `fp_from_ply()` | 将整 ply 转换为 FP 值 |
| `ply_from_fp()` | 将 FP 值转换回整 ply |

### 工作流程

```
1. MovePicker 按顺序产生走法
2. 对每个走法，decide_lmr_quiet() 或 decide_lmr_capture() 计算 reduction
3. 使用 depth - reduction 进行削减窗口搜索
4. 如果削减搜索得分 > alpha：
   → 使用全深度重新搜索（空窗验证）
5. 如果全深度搜索也通过：
   → 这是一个新的最佳走法
```

---

## English

### Overview
`Lmr.cpp` implements the **Late Move Reduction (LMR)** system for the MagnusChess engine. LMR is a core technique for improving search efficiency: later-sorted moves are searched with reduced depth, with the reduction amount dynamically adjusted based on move quality and search context.

### Core Architecture

#### Fractional Ply (`FP_ONE_PLY`)

LMR uses **fractional precision ply** for calculations, where `FP_ONE_PLY = 1024` represents one standard search ply. This allows reductions to be continuously adjustable rather than integer plies:
```
base_depth = original depth (in plies)
reduction = computed reduction amount (in FP units)
search_depth = base_depth - (reduction / FP_ONE_PLY)
```

#### Move Classification

LMR distinguishes two move categories with different reduction strategies:

1. **Quiet move reduction** — Uses the `decide_lmr_quiet()` function family
2. **Capture move reduction** — Uses the `decide_lmr_capture()` function family

### Quiet Move Reduction Logic (`decide_lmr_quiet()`)

**Base reduction formula** (Stockfish-like):
```
reduction = base + ln(depth_factor) × ln(move_index_offset)
```

**Adjustment factors** (reduce the reduction, i.e., give more promising moves less reduction):

| Condition | Adjustment | Reason |
|-----------|------------|--------|
| PV node | Reduce reduction | PV moves deserve more caution |
| Move gives check | Reduce reduction | Checks may force position changes |
| High history score | Reduce reduction (`lmr_history_max`) | High-history moves are more reliable |
| Previous was a killer | Reduce reduction | Killer moves suggest active positions |
| Previous move gave check | Reduce reduction | Dynamic positions need more care |
| Position improving | Reduce reduction | Related to improving score adjustment |

### Capture Reduction Logic (`decide_lmr_capture()`)

Applies additional reduction for captures with SEE < 0, with the reduction proportional to the SEE loss.

### Key Functions

| Function | Description |
|----------|-------------|
| `decide_lmr_quiet()` | Determine LMR reduction amount for quiet moves (in FP units) |
| `decide_lmr_capture()` | Determine LMR reduction amount for captures |
| `lmr_research_depth()` | Compute depth for LMR re-search (if null-window fails) |
| `fp_from_ply()` | Convert integer ply to FP value |
| `ply_from_fp()` | Convert FP value back to integer ply |

### Workflow

```
1. MovePicker produces moves in order
2. For each move, decide_lmr_quiet() or decide_lmr_capture() computes reduction
3. Reduced window search at depth - reduction
4. If reduced search score > alpha:
   → Full-depth re-search (null-window verification)
5. If full-depth search also passes:
   → It's a new best move
```
