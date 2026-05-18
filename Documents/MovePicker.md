# MovePicker.cpp - 走法选择与排序 / Move Picking and Ordering

## 中文

### 概述
`MovePicker.cpp` 实现了 MagnusChess 的分阶段走法选择器。`MovePicker` 负责从走法列表中按优先级顺序逐一产生走法，涵盖从置换表走法到坏吃子的完整排序流水线。

### 分阶段架构

走法按以下**五个阶段**依次产生：

```
阶段 0: TT_MOVE     — 置换表提示走法（优先尝试）
阶段 1: GOOD_CAPTURES — MVV-LVA + SEE 筛选的正值吃子
阶段 2: KILLER_1     — 槽位 0 的杀手走法
阶段 3: KILLER_2     — 槽位 1 的杀手走法
阶段 4: QUIETS       — 余下的安静走法（按历史排序）
阶段 5: BAD_CAPTURES — SEE 负值的吃子（延后尝试）
```

**qsearch** 模式下跳过阶段 2-4（不搜索安静走法）。

### 核心排序逻辑

#### 吃子排序
- **MVV-LVA**：主要排序键 = 受害者的 `see_piece_value`，次要排序键 = 攻击者的 `see_piece_value`
- **SEE 分叉**：根据 `see_ge(move, 0)` 将吃子分为"好"（第一阶段）和"坏"（最后阶段）

#### 安静走法排序
安静走法的得分由 `quiet_ordering_score_fast()` 计算，由五项组成：
1. **安静走法历史**（权重 1）— from→to 的历史得分
2. **反走法奖励**（权重 1/1）— 上一步走法对应的反走法匹配奖励（`MAGNUS_COUNTERMOVE_BONUS = 4096`）
3. **延续历史 1**（权重 1/1）— (prev_piece, prev_to) → (cur_piece, cur_to)
4. **延续历史 2**（权重 1/4）— (prev2_piece, prev2_to) → (cur_piece, cur_to)，衰减更多
5. **兵形历史**（权重 1）— 兵形哈希 × 棋子类型 × 目标格

#### 吃子排序（`capture_ordering_score_fast()`）
1. **吃子历史**（权重 1）— (side, mover, to, captured) 的历史得分
2. **SEE 即时项**（权重 1，Medium 预设）— `clamp(see/2, -150, 150)`
3. **SEE 偏置项**（权重 1）— `clamp(see_bias[dc][sc] / 4, -96, 96)`

### 安静走法限制

`select_quiets()` 采用指数增长的跳过策略：
- 根据 `quiet_limit` 跳过一部分低分安静走法
- 当引擎对第一个安静走法不满意时，逐渐提高限制以包含更多候选

### 关键函数

| 函数 | 描述 |
|------|------|
| `next_move()` | 从当前阶段获取下一个走法 |
| `score_captures()` | 对吃子列表进行 MVV-LVA + SEE 评分 |
| `score_quiets()` | 对安静走法列表进行历史评分 |
| `select_quiets()` | 根据 quiet_limit 筛选安静走法 |
| `stage_next()` | 推进到下一个生成阶段 |

---

## English

### Overview
`MovePicker.cpp` implements MagnusChess's staged move selector. `MovePicker` is responsible for producing moves one by one from move lists in priority order, covering the complete ordering pipeline from TT move to bad captures.

### Staged Architecture

Moves are produced sequentially across **five stages**:

```
Stage 0: TT_MOVE     — Transposition table hint move (tried first)
Stage 1: GOOD_CAPTURES — Positive-SEE captures sorted by MVV-LVA
Stage 2: KILLER_1     — Killer move from slot 0
Stage 3: KILLER_2     — Killer move from slot 1
Stage 4: QUIETS       — Remaining quiet moves (sorted by history)
Stage 5: BAD_CAPTURES — Negative-SEE captures (tried last)
```

In **qsearch** mode, stages 2-4 are skipped (quiet moves are not searched).

### Core Ordering Logic

#### Capture Ordering
- **MVV-LVA**: Primary sort key = victim's `see_piece_value`, secondary = attacker's `see_piece_value`
- **SEE split**: Captures are classified as "good" (stage 1) or "bad" (final stage) via `see_ge(move, 0)`

#### Quiet Move Ordering
The score for quiet moves is computed by `quiet_ordering_score_fast()`, consisting of five terms:
1. **Quiet history** (weight 1) — from→to history score
2. **Countermove bonus** (weight 1/1) — match bonus for the countermove of the previous move (`MAGNUS_COUNTERMOVE_BONUS = 4096`)
3. **Continuation history 1** (weight 1/1) — (prev_piece, prev_to) → (cur_piece, cur_to)
4. **Continuation history 2** (weight 1/4) — (prev2_piece, prev2_to) → (cur_piece, cur_to), more decayed
5. **Pawn history** (weight 1) — pawn hash × piece type × target square

#### Capture Ordering (`capture_ordering_score_fast()`)
1. **Capture history** (weight 1) — (side, mover, to, captured) history score
2. **SEE immediate term** (weight 1, Medium preset) — `clamp(see/2, -150, 150)`
3. **SEE bias term** (weight 1) — `clamp(see_bias[dc][sc] / 4, -96, 96)`

### Quiet Move Limiting

`select_quiets()` employs an exponentially increasing skip strategy:
- Skips a portion of low-scoring quiet moves based on `quiet_limit`
- When the engine is dissatisfied with the first quiet move, gradually raises the limit to include more candidates

### Key Functions

| Function | Description |
|----------|-------------|
| `next_move()` | Get the next move from the current stage |
| `score_captures()` | Score the capture list by MVV-LVA + SEE |
| `score_quiets()` | Score the quiet list by history |
| `select_quiets()` | Filter quiets based on quiet_limit |
| `stage_next()` | Advance to the next generation stage |
