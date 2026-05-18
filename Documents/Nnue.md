# Nnue.cpp - 神经网络评估 / NNUE Evaluation

## 中文

### 概述
`Nnue.cpp` 实现了 MagnusChess 引擎的 **NNUE（Efficiently Updatable Neural Network）** 评估函数。NNUE 是一种高度优化的神经网络架构，专为国际象棋引擎设计，能在 CPU 上实现极快的增量评估。

### 网络架构

#### Chess768 输入方案

NNUE 网络使用 **"Chess768"** 输入布局：
- 输入维度：**768** = (2 色 × 6 种类 + 1 空) × 64 格… 实际为 2 视角 × (6 种棋子 × 64) = 768
- **双视角**：同时编码己方视角和对方视角的棋子布局

#### 网络层结构
```
输入层:   768 neurons (双视角棋盘编码)
隐藏层:   128 neurons (全连接 + 量化激活)
输出层:   1 scalar (评估值，以 CP 为单位)
```

- `kInputSize = 768`
- `kHiddenSize = 128`
- `kActivationClip = 255`（ScreLU 截断阈值）

#### 激活函数：ScreLU

网络使用 **ScreLU (Squared Clipped ReLU)**：
```
ScreLU(x) = clamp(x, 0, 255)²
```
这是一种在 NNUE 中广泛使用的激活函数，结合了 ReLU 的非线性和平方的分布展宽。

### 增量更新原理

NNUE 的核心优势在于**高效增量更新（Efficiently Updatable）**：

1. 每次走法只改变棋盘上 1-3 个位置（from/to/吃子）
2. 只更新与这些位置相关的特征
3. 不需要整个网络重新前向传播
4. 使用 **feature delta refresh** 快速更新输入层激活

### 核心组件

#### 网络加载

支持两种格式：
| 格式 | 扩展名 | 描述 |
|------|--------|------|
| 原生格式 | `.nnue` | MagnusChess 自有网络格式 |
| Bullet 量化 | `.bin` | 量化网络格式，`NetworkFileQuantized` |

`NnueNetwork` 结构体存储所有权重和偏置，通过 `load_nnue_network()` 加载。

#### 前向传播（`forward()`）

传入特征索引集，计算最终评估：
1. 输入层：遍历有效特征索引，累加对应权重（增量）
2. 隐藏层：`ScreLU(input_hidden × weights + bias)`
3. 输出层：`hidden_output × output_weights + output_bias`

#### AVX2 SIMD 加速

关键计算路径使用 **AVX2** 指令集加速：
- 256 位向量化权重累加
- `_mm256_add_epi16`、`_mm256_madd_epi16` 等 SIMD 指令
- 并行处理 16 个 16 位元素

#### 分数转换

| 函数 | 描述 |
|------|------|
| `cp_to_wdl()` | CP（centipawn）→ 胜率和（WDL） |
| `wdl_to_cp()` | 胜率和 → CP |
| `winrate_to_cp()` | 胜率 → CP |

### 关键函数

| 函数 | 描述 |
|------|------|
| `load_nnue_network()` | 从文件加载 NNUE 网络 |
| `forward()` | 执行 NNUE 前向传播 |
| `nnue_evaluate()` | 完整 NNUE 评估（特征提取 + 前向） |
| `init_nnue()` | 初始化 NNUE 系统 |
| `refresh_features()` | 增量更新输入特征 |

### 与手写评估的集成

```
评估选择逻辑：
if (NNUE 可用且 position.count >= threshold):
    eval = nnue_evaluate(pos, mem)
else:
    eval = evaluate(pos, mem)  // HCE 回退
```

---

## English

### Overview
`Nnue.cpp` implements the **NNUE (Efficiently Updatable Neural Network)** evaluation function for the MagnusChess engine. NNUE is a highly optimized neural network architecture designed for chess engines, capable of extremely fast incremental evaluation on CPU.

### Network Architecture

#### Chess768 Input Scheme

The NNUE network uses the **"Chess768"** input layout:
- Input dimension: **768** = 2 perspectives × (6 piece types × 64 squares) = 768
- **Dual perspective**: Simultaneously encodes piece placement from own and opponent views

#### Network Layer Structure
```
Input layer:   768 neurons (dual-perspective board encoding)
Hidden layer:  128 neurons (fully connected + quantized activation)
Output layer:  1 scalar (evaluation in centipawns)
```

- `kInputSize = 768`
- `kHiddenSize = 128`
- `kActivationClip = 255` (ScreLU clamp threshold)

#### Activation Function: ScreLU

The network uses **ScreLU (Squared Clipped ReLU)**:
```
ScreLU(x) = clamp(x, 0, 255)²
```
This is a widely-used activation function in NNUE, combining ReLU's nonlinearity with squared distribution widening.

### Incremental Update Principle

The core advantage of NNUE lies in **efficient incremental updates**:

1. Each move only changes 1-3 positions on the board (from/to/capture)
2. Only features related to these positions need updating
3. No need for a full network forward pass
4. Uses **feature delta refresh** to quickly update input layer activations

### Core Components

#### Network Loading

Supports two formats:
| Format | Extension | Description |
|--------|-----------|-------------|
| Native | `.nnue` | MagnusChess proprietary network format |
| Bullet quantized | `.bin` | Quantized network format, `NetworkFileQuantized` |

The `NnueNetwork` struct stores all weights and biases, loaded via `load_nnue_network()`.

#### Forward Propagation (`forward()`)

Receives feature index sets, computes the final evaluation:
1. Input layer: Iterate over valid feature indices, accumulate corresponding weights (incremental)
2. Hidden layer: `ScreLU(input_hidden × weights + bias)`
3. Output layer: `hidden_output × output_weights + output_bias`

#### AVX2 SIMD Acceleration

Key computation paths are accelerated using **AVX2** instruction set:
- 256-bit vectorized weight accumulation
- SIMD instructions like `_mm256_add_epi16`, `_mm256_madd_epi16`
- Parallel processing of 16 16-bit elements

#### Score Conversion

| Function | Description |
|----------|-------------|
| `cp_to_wdl()` | CP (centipawn) → win-draw-loss |
| `wdl_to_cp()` | WDL → CP |
| `winrate_to_cp()` | Win rate → CP |

### Key Functions

| Function | Description |
|----------|-------------|
| `load_nnue_network()` | Load NNUE network from file |
| `forward()` | Execute NNUE forward pass |
| `nnue_evaluate()` | Full NNUE evaluation (feature extraction + forward) |
| `init_nnue()` | Initialize NNUE system |
| `refresh_features()` | Incrementally update input features |

### Integration with HCE

```
Evaluation selection logic:
if (NNUE available && position.count >= threshold):
    eval = nnue_evaluate(pos, mem)
else:
    eval = evaluate(pos, mem)  // HCE fallback
```
