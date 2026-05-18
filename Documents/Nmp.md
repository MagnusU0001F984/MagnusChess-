# Nmp.cpp - 空走裁剪 / Null Move Pruning

## 中文

### 概述
`Nmp.cpp` 实现了 MagnusChess 引擎的**空走裁剪（Null Move Pruning, NMP）** 系统。NMP 是一种高效的搜索优化技术，通过让对手"连走两步"来检测当前局面是否好得不需要仔细搜索。

### 算法原理

空走裁剪基于一个简单的假设：如果让对手走两步（己方跳过一步），对方仍无法获得优势，则己方当前局面已足够好，可直接剪枝。

### 核心逻辑（`decide_null_move()`）

#### 前置条件检查

空走裁剪在以下情况**不**执行：
1. **是 PV 节点** — 需要保留主变异
2. **在搜索根部** — 不符合空走裁剪的使用场景
3. **是应将状态** — 被将军时跳过一步是非法操作
4. **beta >= VALUE_MATE** — 已接近将杀分数
5. **己方子力太少** — 残局中空走裁剪效果差

#### 空走深度计算

```
R = base_R + depth / scale_factor
```

其中 `base_R` 是基础减免（通常 3-4 ply），`depth / scale_factor` 是动态部分。更深搜索获得更大的空走减免。

#### 验证搜索

当主空走搜索得分 >= beta 时：

**浅层验证（通常 depth ≤ 6）**：直接剪枝，不做额外验证。

**深层验证（depth > threshold）**：执行**验证搜索**（verification search），使用空走减免后的深度再做一次正常搜索。如果验证搜索仍 >= beta，才确认剪枝。

这避免了深层搜索中空走裁剪的误判（如某些 zugzwang 局面）。

### 空走状态管理

`null_move_state` 是一个辅助实用函数，管理 NMP 搜索时是否需要更新与空走相关的搜索状态标志。

### 关键函数

| 函数 | 描述 |
|------|------|
| `decide_null_move()` | 决定是否执行 NMP 以及计算空走深度 |

### 与 PVS 的集成

```
pvs(alpha, beta, depth, ply) {
    if (decide_null_move(pos, depth, beta, ss, ply)) {
        pos.make_null_move();
        null_score = -pvs(-beta, -beta+1, depth-R-1, ply+1);
        pos.unmake_null_move();

        if (null_score >= beta) {
            if (depth > verification_threshold)
                verification_score = pvs(alpha, beta, depth-R-1, ply);
            if (verification_score >= beta || depth <= verification_threshold)
                return null_score;  // 剪枝
        }
    }
    ...
}
```

### 设计要点
- NMP 节省大量搜索时间，特别是在中局阶段
- 验证搜索防止 zugzwang 或战术密集局面中的误剪
- 子力阈值检查避免残局 zugzwang 风险
- 动态缩减公式使深层节点获得更激进的裁剪

---

## English

### Overview
`Nmp.cpp` implements the **Null Move Pruning (NMP)** system for the MagnusChess engine. NMP is an efficient search optimization technique that detects whether the current position is good enough to skip detailed search by letting the opponent "move twice."

### Algorithm Principle

Null move pruning is based on a simple assumption: if letting the opponent move twice (skipping one's own turn) still fails to give them an advantage, then the current position is already good enough for a cutoff.

### Core Logic (`decide_null_move()`)

#### Precondition Checks

Null move pruning is **not** executed when:
1. **PV node** — The principal variation must be preserved
2. **At the search root** — Not applicable for NMP
3. **In check** — Skipping a move while in check is illegal
4. **beta >= VALUE_MATE** — Already near checkmate scores
5. **Insufficient non-pawn material** — NMP is less effective in endgames

#### Null Move Depth Calculation

```
R = base_R + depth / scale_factor
```

Where `base_R` is the base reduction (typically 3-4 plies) and `depth / scale_factor` is the dynamic portion. Deeper searches get larger null move reductions.

#### Verification Search

When the main null move search returns a score >= beta:

**Shallow verification (typically depth ≤ 6)**: Direct cutoff, no additional verification.

**Deep verification (depth > threshold)**: Execute a **verification search**, performing a normal search at the null-reduced depth. Only confirm the cutoff if the verification search still returns >= beta.

This prevents false cutoffs from NMP in deep searches (e.g., certain zugzwang positions).

### Null Move State Management

`null_move_state` is a helper utility that manages whether null-move-related search state flags need updating during NMP.

### Key Functions

| Function | Description |
|----------|-------------|
| `decide_null_move()` | Decide whether to perform NMP and compute null move depth |

### Integration with PVS

```
pvs(alpha, beta, depth, ply) {
    if (decide_null_move(pos, depth, beta, ss, ply)) {
        pos.make_null_move();
        null_score = -pvs(-beta, -beta+1, depth-R-1, ply+1);
        pos.unmake_null_move();

        if (null_score >= beta) {
            if (depth > verification_threshold)
                verification_score = pvs(alpha, beta, depth-R-1, ply);
            if (verification_score >= beta || depth <= verification_threshold)
                return null_score;  // cutoff
        }
    }
    ...
}
```

### Design Notes
- NMP saves significant search time, especially in middlegame positions
- Verification search prevents false cutoffs in zugzwang or tactically dense positions
- Material threshold check avoids endgame zugzwang risks
- Dynamic reduction formula allows more aggressive pruning at deeper nodes
