# TT.cpp - 置换表 / Transposition Table

## 中文

### 概述
`TT.cpp` 实现了 MagnusChess 引擎的**置换表（Transposition Table, TT）**，这是搜索系统的核心缓存结构。它使用 4 路组相联哈希桶、无锁原子操作和节电替换策略，为搜索提供高效的局面缓存。

### 核心架构

#### 数据结构

**`TTCluster`** — 4 路组相联桶，每个桶包含 4 个 `TTEntry`：
```
struct TTCluster {
    TTEntry entries[4];
};
```

**`TTEntry`** 包含：
- `key16` — Zobrist key 的高 16 位（签名），用于快速匹配
- `move` — 最佳走法
- `score` — 搜索分数
- `eval` — 静态评估
- `depth` — 搜索深度
- `generation` — 年龄（用于替换决策，2 位）
- `bound` — 边界类型（精确值/下界/上界）
- `pad` — 对齐填充

**`TT`** 结构体：
- `clusters` — `TTCluster` 数组（总大小 = `hash_mb / sizeof(TTCluster)` MB）
- `hash_mask` — 桶索引掩码

#### 哈希索引方案

```
bucket_index = hash_key & hash_mask
cluster = &clusters[bucket_index]
```
在 4 路桶中线性搜索匹配的 `key16`。

#### 无锁原子操作

TT 使用 `std::atomic_ref<i64>` 对每个条目进行原子读写，允许在多线程搜索中无锁并行访问（lockless hashing）。写入是原子的，读取不需要锁定。

### 核心算法

#### 探测（`tt_probe()`）
1. 计算 `bucket_index = key & hash_mask`
2. 遍历桶中 4 个条目
3. 找到 `key16` 匹配的条目 → 返回绑定类型、分数、深度、走法和评估
4. 同时返回最佳替换槽（考虑深度和年龄）

#### 保存（`tt_save()`）
替换策略：
1. 优先覆盖空槽位
2. 在有占用的槽位中，选择最"可替换"的：
   - 来自旧搜索世代的条目优先替换
   - 浅深度条目优先替换
   - 综合评分 = `depth - 4 * generation_delta`

#### 哈希满度（`tt_hashfull()`）
通过对前 1000 个桶采样，估算 TT 的占用率（permille），用于 UCI 报告。

### 配合搜索的使用方式

```
tt_probe(tt, pos.state_key, tt_hit)
    ↓
if (tt_hit && tt_score >= beta) return tt_score;  // β 剪枝
    ↓
... 搜索 ...
    ↓
tt_save(tt, pos.state_key, best_move, best_score, depth, bound, generation);
```

### 设计要点
- 无锁设计（lockless hashing）支持多线程并发读写
- 4 路组相联减少冲突未命中
- 2 位 generation 字段实现轻量级年龄跟踪
- 原子 64 位写入/读取保证条目一致性

---

## English

### Overview
`TT.cpp` implements the **Transposition Table (TT)** for the MagnusChess engine, the core caching structure of the search system. It uses 4-way set-associative hash buckets, lockless atomic operations, and a conservative replacement policy to provide efficient position caching.

### Core Architecture

#### Data Structures

**`TTCluster`** — 4-way set-associative bucket, each bucket contains 4 `TTEntry`:
```
struct TTCluster {
    TTEntry entries[4];
};
```

**`TTEntry`** contains:
- `key16` — Upper 16 bits of Zobrist key (signature) for fast matching
- `move` — Best move
- `score` — Search score
- `eval` — Static evaluation
- `depth` — Search depth
- `generation` — Age (for replacement decisions, 2 bits)
- `bound` — Bound type (exact / lower bound / upper bound)
- `pad` — Alignment padding

**`TT`** struct:
- `clusters` — Array of `TTCluster` (total size = `hash_mb / sizeof(TTCluster)` MB)
- `hash_mask` — Mask for bucket indexing

#### Hash Indexing Scheme

```
bucket_index = hash_key & hash_mask
cluster = &clusters[bucket_index]
```
Linear search within the 4-way bucket for matching `key16`.

#### Lockless Atomic Operations

TT uses `std::atomic_ref<i64>` for atomic reads/writes of each entry, enabling lockless parallel access in multi-threaded search. Writes are atomic, reads require no locking.

### Core Algorithms

#### Probing (`tt_probe()`)
1. Compute `bucket_index = key & hash_mask`
2. Iterate over 4 entries in the bucket
3. Find entry with matching `key16` → return bound type, score, depth, move, and evaluation
4. Also return the best replacement slot (considering depth and age)

#### Saving (`tt_save()`)
Replacement policy:
1. Prefer overwriting empty slots
2. Among occupied slots, select the most "replaceable":
   - Entries from older search generations are preferred for replacement
   - Shallow depth entries are preferred
   - Composite score = `depth - 4 * generation_delta`

#### Hash Full (`tt_hashfull()`)
Estimates TT occupancy (permille) by sampling the first 1000 buckets, used for UCI reporting.

### Usage with Search

```
tt_probe(tt, pos.state_key, tt_hit)
    ↓
if (tt_hit && tt_score >= beta) return tt_score;  // β cutoff
    ↓
... search ...
    ↓
tt_save(tt, pos.state_key, best_move, best_score, depth, bound, generation);
```

### Design Notes
- Lockless design (lockless hashing) supports multi-threaded concurrent reads/writes
- 4-way set-associativity reduces conflict misses
- 2-bit generation field provides lightweight age tracking
- Atomic 64-bit writes/reads guarantee entry consistency
