# MoveGen.cpp - 走法生成 / Move Generation

## 中文

### 概述
`MoveGen.cpp` 是 MagnusChess 引擎中最大的单一源文件之一（约 49KB），实现了完整的国际象棋走法生成系统。它支持多种生成模式、走法合法性验证、将军检测和特殊走法处理。

### 核心架构

#### 走法模式（PieceMoveMode）

| 模式 | 描述 |
|------|------|
| `NON_EVASIONS` | 非应将状态的所有走法 |
| `CAPTURES` | 仅吃子走法（含升变吃子） |
| `QUIETS` | 仅安静走法（不含吃子） |
| `EVASIONS` | 应将走法（王逃、垫、吃攻击子） |

#### 生成信息结构（GenInfo）
在生成走法之前，`init_gen_info()` 计算以下上下文信息：
- **checkers** — 将军攻击者位棋盘（0/1/2 个）
- **danger** — 所有受攻击方棋子攻击的格子（王不能进入）
- **capture_mask** — 非应将时可吃的棋子（= 对方占领集）
- **push_mask** — 非应将时兵可推进到的目标格
- **pinned** — 被固定棋子的位棋盘
- **pinners** — 固定对方的滑行棋子位棋盘

#### 模板化生成

走法生成通过模板函数实现，模式作为模板参数，确保零运行时开销：

- **`gen_non_evasions()`** — 生成伪合法走法，过滤掉绝对固定违规
  - 王：正常移动（排除 danger 区域） + 易位
  - 兵：单步/双步/吃子/升变/吃过路兵
  - 马：跳马至非友方格
  - 象/车/后：滑行攻击掩码

- **`gen_captures()`** — 仅生成吃子走法
  - 兵：capture_mask 内的吃子/升变吃子 + 吃过路兵
  - 其他：仅生成落点在 capture_mask 内的走法
  - 马/王：循环检查目标是否在 capture_mask 内

- **`gen_quiets()`** — 仅生成不吃子的走法

- **`gen_evasions()`** — 应将模式
  - 双将 → 只能王逃
  - 单将 → 王逃 OR 垫/吃攻击子

#### 合法性检查

| 函数 | 描述 |
|------|------|
| `legal_fast()` | 快速合法性测试（非王走法检查固定违规；王走法检查 danger） |
| `legal_slow()` | 完整合法性测试（执行走法后用 `is_in_check()` 验证） |
| `pseudo_legal_fast()` | 伪合法性测试 |

#### 将军检测（`move_gives_check()`）
增量检查走法是否导致将军：
- 直接将军：走子直接攻击对方王
- 发现将军：走子移开后暴露滑行棋子攻击
- 特殊走法（易位、升变、吃过路兵）单独处理

### 公共 API

| 函数 | 描述 |
|------|------|
| `generate()` | 生成全部走法 |
| `generate_legal()` | 仅生成合法走法 |
| `generate_captures()` | 仅生成吃子走法 |
| `generate_quiets()` | 仅生成安静走法 |
| `generate_evasions()` | 生成应将走法 |

### 设计要点
- 模板化架构确保编译器可为每种模式生成最优代码
- 将军检测采用增量方式避免完整走法执行
- 固定检测为`compute_pinners_and_pinned()`（利用滑行棋子的 X 射线攻击）
- 吃过路兵合法性需额外验证（移走兵后是否仍被将军）

---

## English

### Overview
`MoveGen.cpp` is one of the largest single source files in the MagnusChess engine (~49KB), implementing a complete chess move generation system. It supports multiple generation modes, move legality validation, check detection, and special move handling.

### Core Architecture

#### Move Modes (PieceMoveMode)

| Mode | Description |
|------|-------------|
| `NON_EVASIONS` | All moves when not in check |
| `CAPTURES` | Capture moves only (including promotion captures) |
| `QUIETS` | Quiet moves only (non-captures) |
| `EVASIONS` | Check evasion moves (king escape, block, capture attacker) |

#### Generation Info Structure (GenInfo)
Before generating moves, `init_gen_info()` computes the following context:
- **checkers** — Bitboard of pieces giving check (0/1/2)
- **danger** — All squares attacked by opponent pieces (king cannot enter)
- **capture_mask** — Pieces that can be captured in non-evasion mode (= opponent occupancy)
- **push_mask** — Target squares pawns can push to in non-evasion mode
- **pinned** — Bitboard of pinned pieces
- **pinners** — Bitboard of sliding pieces pinning opponent pieces

#### Template-Based Generation

Move generation uses template functions with mode as a template parameter, ensuring zero runtime overhead:

- **`gen_non_evasions()`** — Generates pseudo-legal moves, filtering out absolute pin violations
  - King: Normal moves (excluding danger zone) + castling
  - Pawns: Single/double pushes, captures, promotions, en passant
  - Knights: Jumps to non-friendly squares
  - Bishops/Rooks/Queens: Sliding attack masks

- **`gen_captures()`** — Generates capture moves only
  - Pawns: Captures/promotion captures within capture_mask + en passant
  - Others: Only moves landing on squares in capture_mask
  - Knights/Kings: Loop check whether target is in capture_mask

- **`gen_quiets()`** — Generates only non-capture moves

- **`gen_evasions()`** — Check evasion mode
  - Double check → King escape only
  - Single check → King escape OR block/capture the checking piece

#### Legality Checking

| Function | Description |
|----------|-------------|
| `legal_fast()` | Fast legality test (non-king moves check pin violation; king moves check danger) |
| `legal_slow()` | Full legality test (executes the move and verifies with `is_in_check()`) |
| `pseudo_legal_fast()` | Pseudo-legality test |

#### Check Detection (`move_gives_check()`)
Incrementally checks whether a move gives check:
- Direct check: Moving piece directly attacks opponent king
- Discovered check: Moving piece reveals a slider attack on opponent king
- Special moves (castling, promotion, en passant) handled separately

### Public API

| Function | Description |
|----------|-------------|
| `generate()` | Generate all moves |
| `generate_legal()` | Generate only legal moves |
| `generate_captures()` | Generate only capture moves |
| `generate_quiets()` | Generate only quiet moves |
| `generate_evasions()` | Generate check evasion moves |

### Design Notes
- Template architecture ensures the compiler can generate optimal code for each mode
- Check detection is incremental to avoid full move execution
- Pin detection uses `compute_pinners_and_pinned()` (leveraging X-ray attacks of sliding pieces)
- En passant legality requires additional verification (king still in check after removing both pawns)
