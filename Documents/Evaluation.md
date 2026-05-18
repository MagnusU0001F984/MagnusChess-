# Evaluation.cpp - 手写评估函数 / Handcrafted Evaluation

## 中文

### 概述
`Evaluation.cpp` 实现了 MagnusChess 引擎的**手写评估函数（Handcrafted Evaluation, HCE）**。虽然引擎也包含 NNUE 评估，HCE 仍被用于增量评估的初始化、NNUE 回退和快速粗略评估场景。

### 核心架构

#### 增量评估

`Position` 结构体维护两个增量评估分数：
- `psqt_score[MG]` — 中局 PSQT（Piece-Square Table）分数
- `psqt_score[EG]` — 残局 PSQT 分数
- `phase` — 局面阶段值（用于中局/残局插值）

增量钩子函数在 `make_move()`/`unmake_move()` 中自动调用：
- `on_piece_added()` / `on_piece_removed()` — 添加/移除棋子时更新 PSQT
- `on_piece_moved()` — 移动棋子时更新 PSQT（等效于移除 + 添加）

#### 棋子价值与阶段

| 棋子 | MG 价值 | EG 价值 | 阶段增量 |
|------|---------|---------|----------|
| 兵 | 82 | 101 | 0 |
| 马 | 337 | 416 | 1 |
| 象 | 365 | 443 | 1 |
| 车 | 477 | 621 | 2 |
| 后 | 1025 | 1204 | 4 |
| 王 | 0 | 0 | 0 |

#### PSQT 表

每种棋子有两张 8×8 的 PSQT 表（中局和残局各一张），从白方视角定义：
- 兵 PSQT：鼓励中心化和通路兵推进
- 马/象 PSQT：中心化奖励
- 车 PSQT：奖励开放线和第七横排
- 后 PSQT：同马/象，中心化
- 王 PSQT：中局奖励王翼安全，残局奖励中心化

黑方通过翻转得分表使用。

#### 阶段插值公式

```
tapered = (mg * phase + eg * (24 - phase)) / 24
```

24 是初始阶段值（所有棋子都在时的 phase 值），范围 0-24。

#### 完整评估（`evaluate()`）

除了增量 PSQT 外，完整评估还计算：
- **兵形** — 使用兵形表缓存
- **材料** — 通过材料表查找修正

### 关键函数

| 函数 | 描述 |
|------|------|
| `evaluate()` | 完整 HCE 评估（PSQT + 兵 + 材料） |
| `on_piece_added()` | 增量更新：棋子被添加 |
| `on_piece_removed()` | 增量更新：棋子被移除 |
| `on_piece_moved()` | 增量更新：棋子被移动 |
| `init_evaluation()` | 初始化所有 PSQT 表 |

### 设计要点
- PSQT 值直接嵌入增量评估中，`make_move` 和 `unmake_move` 自动维护，零开销
- 手写评估与 NNUE 评估互补，在 NNUE 无法使用时提供合理估值
- 阶段插值平滑过渡中局和残局特征

---

## English

### Overview
`Evaluation.cpp` implements the **Handcrafted Evaluation (HCE)** for the MagnusChess engine. Although the engine also includes NNUE evaluation, HCE is still used for incremental evaluation initialization, NNUE fallback, and fast coarse evaluation scenarios.

### Core Architecture

#### Incremental Evaluation

The `Position` struct maintains two incremental evaluation scores:
- `psqt_score[MG]` — Midgame PSQT (Piece-Square Table) score
- `psqt_score[EG]` — Endgame PSQT score
- `phase` — Position phase value (for midgame/endgame interpolation)

Incremental hook functions are automatically called in `make_move()`/`unmake_move()`:
- `on_piece_added()` / `on_piece_removed()` — Update PSQT when pieces are added/removed
- `on_piece_moved()` — Update PSQT when pieces are moved (equivalent to remove + add)

#### Piece Values and Phase

| Piece | MG Value | EG Value | Phase Increment |
|-------|----------|----------|-----------------|
| Pawn | 82 | 101 | 0 |
| Knight | 337 | 416 | 1 |
| Bishop | 365 | 443 | 1 |
| Rook | 477 | 621 | 2 |
| Queen | 1025 | 1204 | 4 |
| King | 0 | 0 | 0 |

#### PSQT Tables

Each piece type has two 8×8 PSQT tables (one for midgame, one for endgame), defined from White's perspective:
- Pawn PSQT: Encourages centralization and passed pawn advancement
- Knight/Bishop PSQT: Centralization bonus
- Rook PSQT: Rewards open files and 7th rank
- Queen PSQT: Similar to knight/bishop, centralization
- King PSQT: Rewards kingside safety in midgame, centralization in endgame

Black uses flipped score tables.

#### Phase Interpolation Formula

```
tapered = (mg * phase + eg * (24 - phase)) / 24
```

24 is the initial phase value (phase when all pieces are present), range 0-24.

#### Full Evaluation (`evaluate()`)

Beyond incremental PSQT, full evaluation also computes:
- **Pawn structure** — Using pawn structure table cache
- **Material** — Correction via material table lookup

### Key Functions

| Function | Description |
|----------|-------------|
| `evaluate()` | Full HCE evaluation (PSQT + pawns + material) |
| `on_piece_added()` | Incremental update: piece added |
| `on_piece_removed()` | Incremental update: piece removed |
| `on_piece_moved()` | Incremental update: piece moved |
| `init_evaluation()` | Initialize all PSQT tables |

### Design Notes
- PSQT values are embedded directly in incremental evaluation, automatically maintained by `make_move`/`unmake_move` with zero overhead
- HCE complements NNUE evaluation, providing reasonable estimates when NNUE is unavailable
- Phase interpolation smoothly transitions between midgame and endgame characteristics
