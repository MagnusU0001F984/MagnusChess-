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

#include "TT.h"

#include <bit>
#include <cstring>

/*
The TT implementation focuses on a cheap probe path: compare four 32-bit tags
at once, prefer empty lanes when possible, and otherwise replace the weakest
entry based on depth, age, and bound quality.
*/

namespace valerain::memory {

constexpr int TT_CLUSTER_SIZE = 4;

namespace {

[[nodiscard]] inline u32 tt_tag32_from_key(Key key) noexcept {
    return static_cast<u32>(key >> 32);
}

[[nodiscard]] inline int lane_mask4_eq_u32_sse(const u32* ptr, u32 tag32) noexcept {
    // Compare the four lane tags in parallel and return a 4-bit match mask.
    const __m128i tags = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
    const __m128i want = _mm_set1_epi32(static_cast<int>(tag32));
    const __m128i cmp  = _mm_cmpeq_epi32(tags, want);
    return _mm_movemask_ps(_mm_castsi128_ps(cmp));
}

[[nodiscard]] inline int first_lane_from_mask4(int mask) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(mask);
#else
    int lane = 0;
    while (((mask >> lane) & 1) == 0) ++lane;
    return lane;
#endif
}

[[nodiscard]] inline int empty_lane_mask4_u8(const u8* ages) noexcept {
    u32 x = 0;
    std::memcpy(&x, ages, sizeof(x));

    const u32 z = (x - 0x01010101u) & ~x & 0x80808080u;

    return ((z >> 7)  & 0x1)
         | (((z >> 15) & 0x1) << 1)
         | (((z >> 23) & 0x1) << 2)
         | (((z >> 31) & 0x1) << 3);
}

[[nodiscard]] inline int first_live_match_lane4(
    const TTCluster& c,
    int tag_mask
) noexcept {
    if ((tag_mask & 0x1) && c.age[0] != 0) return 0;
    if ((tag_mask & 0x2) && c.age[1] != 0) return 1;
    if ((tag_mask & 0x4) && c.age[2] != 0) return 2;
    if ((tag_mask & 0x8) && c.age[3] != 0) return 3;
    return -1;
}

[[nodiscard]] inline int replacement_score_lane(
    const TTCluster& c,
    int lane,
    u8 current_age
) noexcept {
    // Deeper, newer, exact, and PV entries are more expensive to overwrite.
    const int age_penalty = static_cast<int>(static_cast<u8>(current_age - c.age[lane]));
    const int exact_bonus = ((c.flags[lane] & 0x3U) == BOUND_EXACT) ? 8 : 0;
    const int pv_bonus    = (c.flags[lane] & 0x4U) ? 4 : 0;

    return static_cast<int>(c.depth[lane]) - age_penalty * 2 + exact_bonus + pv_bonus;
}

[[nodiscard]] inline int best_replacement_lane4(
    const TTCluster& c,
    u8 current_age
) noexcept {
    int best_lane = 0;
    int best_score = replacement_score_lane(c, 0, current_age);

    const int s1 = replacement_score_lane(c, 1, current_age);
    if (s1 < best_score) {
        best_score = s1;
        best_lane = 1;
    }

    const int s2 = replacement_score_lane(c, 2, current_age);
    if (s2 < best_score) {
        best_score = s2;
        best_lane = 2;
    }

    const int s3 = replacement_score_lane(c, 3, current_age);
    if (s3 < best_score) {
        best_score = s3;
        best_lane = 3;
    }

    return best_lane;
}

} // namespace

TTData tt_cluster_load(const TTCluster& c, int lane) noexcept {
    TTData d;
    d.tag32 = c.tag32[lane];
    d.move  = c.move[lane];
    d.score = c.score[lane];
    d.eval  = c.eval[lane];
    d.depth = c.depth[lane];
    d.age   = c.age[lane];
    d.flags = c.flags[lane];
    d.spare = c.spare[lane];
    return d;
}

void tt_cluster_store(TTCluster& c, int lane, const TTData& d) noexcept {
    c.move[lane]  = d.move;
    c.score[lane] = d.score;
    c.eval[lane]  = d.eval;
    c.depth[lane] = d.depth;
    c.age[lane]   = d.age;
    c.flags[lane] = d.flags;
    c.spare[lane] = d.spare;
    c.tag32[lane] = d.tag32;
}

void tt_cluster_clear(TTCluster& c) noexcept {
    std::memset(&c, 0, sizeof(TTCluster));
}

int tt_replacement_score(const TTCluster& c, int lane, u8 current_age) noexcept {
    if (c.age[lane] == 0)
        return -1000000000;

    return replacement_score_lane(c, lane, current_age);
}

void tt_free(TT& tt) noexcept {
    delete[] tt.clusters;
    tt.clusters = nullptr;
    tt.cluster_count = 0;
    tt.cluster_mask = 0;
    tt.generation = 1;
}

void tt_clear(TT& tt) noexcept {
    if (!tt.clusters) return;
    std::memset(tt.clusters, 0, sizeof(TTCluster) * tt.cluster_count);
    tt.generation = 1;
}

void tt_resize_mb(TT& tt, std::size_t mb) {
    std::size_t bytes = mb * 1024ULL * 1024ULL;
    std::size_t count = bytes / sizeof(TTCluster);
    if (count == 0) count = 1;
    count = std::bit_ceil(count);

    TTCluster* new_clusters = new TTCluster[count]{};
    TTCluster* old_clusters = tt.clusters;

    tt.clusters = new_clusters;
    tt.cluster_count = count;
    tt.cluster_mask = count - 1;
    tt.generation = 1;

    delete[] old_clusters;
}

void tt_new_search(TT& tt) noexcept {
    ++tt.generation;
    if (tt.generation == 0) tt.generation = 1;
}

std::size_t tt_index(const TT& tt, Key key) noexcept {
    return mix64(key) & tt.cluster_mask;
}

void tt_prefetch(const TT& tt, Key key) noexcept {
    if (!tt.clusters) return;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&tt.clusters[tt_index(tt, key)], 0, 3);
#elif defined(_MSC_VER)
    _mm_prefetch(reinterpret_cast<const char*>(&tt.clusters[tt_index(tt, key)]), _MM_HINT_T0);
#endif
}

TTProbe tt_probe(TT& tt, Key key) noexcept {
    // Probe returns both the hit data (if any) and the slot that should be
    // updated on save, so the caller only hashes once.
    TTProbe res{};
    if (!tt.clusters) return res;

    TTCluster& c = tt.clusters[tt_index(tt, key)];
    const u32 tag32 = tt_tag32_from_key(key);

    const int tag_mask = lane_mask4_eq_u32_sse(c.tag32, tag32);
    if (tag_mask) {
        const int hit_lane = first_live_match_lane4(c, tag_mask);
        if (hit_lane >= 0) {
            res.hit = true;
            res.slot.cluster = &c;
            res.slot.lane = hit_lane;
            res.data = tt_cluster_load(c, hit_lane);
            return res;
        }
    }

    const int empty_mask = empty_lane_mask4_u8(c.age);
    if (empty_mask) {
        const int lane = first_lane_from_mask4(empty_mask);
        res.slot.cluster = &c;
        res.slot.lane = lane;
        return res;
    }

    const int lane = best_replacement_lane4(c, tt.generation);
    res.slot.cluster = &c;
    res.slot.lane = lane;
    return res;
}

void tt_save(
    TT& tt,
    Key key,
    Move move,
    i16 score,
    i16 eval,
    i16 depth,
    Bound bound,
    bool pv
) noexcept {
    // Replacement is conservative when the existing entry is deeper and more exact.
    if (!tt.clusters) return;

    TTProbe pr = tt_probe(tt, key);
    TTData old = pr.hit ? pr.data : TTData{};

    if (move == 0 && pr.hit)
        move = old.move;

    if (pr.hit) {
        const Bound old_bound = static_cast<Bound>(old.flags & 0x3U);
        const bool old_pv = (old.flags & 0x4U) != 0;

        const bool weaker =
            depth < old.depth &&
            bound != BOUND_EXACT &&
            old_bound == BOUND_EXACT &&
            !pv &&
            old_pv;

        if (weaker) return;
    }

    TTData nw;
    nw.tag32 = tt_tag32_from_key(key);
    nw.move  = move;
    nw.score = score;
    nw.eval  = eval;
    nw.depth = depth;
    nw.age   = tt.generation;
    nw.flags = static_cast<u8>(bound) | (pv ? 0x4U : 0U);
    nw.spare = 0;

    tt_cluster_store(*pr.slot.cluster, pr.slot.lane, nw);
}

int tt_hashfull(const TT& tt, int sample_clusters) noexcept {
    if (!tt.clusters || tt.cluster_count == 0) return 0;

    const int n = static_cast<int>(std::min<std::size_t>(
        tt.cluster_count, static_cast<std::size_t>(sample_clusters)
    ));

    int used = 0;
    int total = 0;

    for (int i = 0; i < n; ++i) {
        const TTCluster& c = tt.clusters[i];
        for (int lane = 0; lane < TT_CLUSTER_SIZE; ++lane) {
            ++total;
            used += (c.age[lane] == tt.generation);
        }
    }

    return total ? (used * 1000) / total : 0;
}

} // namespace valerain::memory
