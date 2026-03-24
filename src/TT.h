/*
MIT License

Copyright (c) 2026 Mazhaoze

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

#include "Types.h"
#include "Tables.h"

namespace valerain::memory {

using ::valerain::u8;
using ::valerain::u16;
using ::valerain::u32;
using ::valerain::u64;
using ::valerain::i16;
using ::valerain::Move;
using ::valerain::Key;
using ::valerain::mix64;

enum Bound : u8 {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,
    BOUND_LOWER = 2,
    BOUND_EXACT = 3
};

struct TTData {
    u32 tag32 = 0;
    u16 move  = 0;
    i16 score = 0;
    i16 eval  = 0;
    i16 depth = 0;
    u8  age   = 0;
    u8  flags = 0;
    u16 spare = 0;
};

struct alignas(64) TTCluster {
    u32 tag32[4]{};
    u16 move[4]{};
    i16 score[4]{};
    i16 eval[4]{};
    i16 depth[4]{};
    u8  age[4]{};
    u8  flags[4]{};
    u16 spare[4]{};
};

static_assert(sizeof(TTCluster) == 64, "TTCluster must be 64 bytes.");

struct TT {
    TTCluster* clusters = nullptr;
    std::size_t cluster_count = 0;
    std::size_t cluster_mask = 0;

    std::atomic_flag* locks = nullptr;
    std::size_t lock_count = 0;
    std::size_t lock_mask = 0;

    u8 generation = 1;
};

struct TTSlotRef {
    TTCluster* cluster = nullptr;
    int lane = 0;
};

struct TTProbe {
    bool hit = false;
    TTSlotRef slot{};
    TTData data{};
};

[[nodiscard]] TTData tt_cluster_load(const TTCluster& c, int lane) noexcept;
void tt_cluster_store(TTCluster& c, int lane, const TTData& d) noexcept;
void tt_cluster_clear(TTCluster& c) noexcept;

[[nodiscard]] int tt_replacement_score(const TTCluster& c, int lane, u8 current_age) noexcept;

void tt_free(TT& tt) noexcept;
void tt_clear(TT& tt) noexcept;
void tt_resize_mb(TT& tt, std::size_t mb);
void tt_new_search(TT& tt) noexcept;

[[nodiscard]] std::size_t tt_index(const TT& tt, Key key) noexcept;
void tt_prefetch(const TT& tt, Key key) noexcept;
[[nodiscard]] TTProbe tt_probe(TT& tt, Key key) noexcept;

void tt_save(
    TT& tt,
    Key key,
    Move move,
    i16 score,
    i16 eval,
    i16 depth,
    Bound bound,
    bool pv
) noexcept;

[[nodiscard]] int tt_hashfull(const TT& tt, int sample_clusters = 1000) noexcept;

} // namespace valerain::memory
