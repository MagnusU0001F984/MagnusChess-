/*
MIT License

Copyright (c) 2026 MagnusU0001F984

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

#include "Lmr.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "TT.h"

/*
 * LMR (延遲著法減免) 實作 — Late Move Reduction
 *
 * 這是搜尋引擎最關鍵的剪枝技術。核心原理：
 *   排序靠後的著法用縮減深度搜索，節省大量節點。
 *   若縮減搜索意外超過 alpha，觸發 re-search 確認。
 *
 * 減免量使用固定點 (fixed-point) 算術，FP_ONE_PLY=1024 代表 1 個 ply。
 * 基礎減免 = log(depth) * log(move_index)，通過查表實現，
 * 然後根據節點類型、改善狀態、TT 狀態、歷史分數等進行 ± 調整。
 *
 * 兩個公開函數：
 *   decide_lmr()        — 計算減免決策（是否減免、減免量）
 *   lmr_research_depth() — 計算 re-search 深度（減免搜索超過 alpha 時）
 */
namespace magnus::search {

namespace {

// FP_ONE_PLY = 1024：固定點精度，1 個 ply = 1024 單位
// 使用固定點而非浮點避免精度問題並加速計算
constexpr int FP_ONE_PLY = 1024;
// LMR 查表最大索引 — 對數表的預計算範圍
constexpr int LMR_TABLE_MAX_INDEX = 64;
constexpr double LMR_TABLE_LOG_SCALE = 2747.0 / 128.0;
constexpr int QUIET_HISTORY_FP_DIVISOR = 12;
constexpr int CAPTURE_HISTORY_FP_DIVISOR = 16;
constexpr int LMR_DEEPER_RESEARCH_MARGIN = 96;
constexpr int LMR_SHALLOWER_RESEARCH_MARGIN = 8;

[[nodiscard]] const std::array<int, LMR_TABLE_MAX_INDEX + 1>& lmr_table() noexcept {
    static const std::array<int, LMR_TABLE_MAX_INDEX + 1> table = []() {
        std::array<int, LMR_TABLE_MAX_INDEX + 1> values{};
        for (int i = 1; i <= LMR_TABLE_MAX_INDEX; ++i)
            values[i] = std::max(1, static_cast<int>(LMR_TABLE_LOG_SCALE * std::log(double(i))));
        return values;
    }();
    return table;
}

[[nodiscard]] static inline int lmr_table_value(int index) noexcept {
    return lmr_table()[std::clamp(index, 1, LMR_TABLE_MAX_INDEX)];
}

[[nodiscard]] static inline int base_quiet_reduction_fp(int depth, int move_index) noexcept {
    const int d = lmr_table_value(depth);
    const int m = lmr_table_value(move_index + 1);
    return d * m + FP_ONE_PLY / 2;
}

[[nodiscard]] static inline int base_capture_reduction_fp(int depth, int reduction_index) noexcept {
    const int d = lmr_table_value(depth);
    const int m = lmr_table_value(reduction_index + 1);
    return std::max(0, (d * m * 3) / 4 - FP_ONE_PLY / 4);
}

// Depth-adaptive continuation weighting: shallow continuation signals
// are noisier, so scale them down relative to the search depth.
[[nodiscard]] static inline int quiet_stat_score(
    const LmrMoveContext& move, int depth
) noexcept {
    const int depth_factor = std::min(320, 128 + depth * 16); // depth=4→192, depth=12→320
    const int weighted_continuation = move.continuation_score * depth_factor / 256;
    return std::clamp(
        2 * move.quiet_history_score
            + weighted_continuation
            + move.countermove_bonus / 2
            + move.ordering_score / 8,
        -16384,
        16384
    );
}

[[nodiscard]] static inline int capture_stat_score(const LmrMoveContext& move) noexcept {
    return std::clamp(
        move.capture_history_score
            + move.see_bias_term * 16
            + std::clamp(move.see_value, -512, 512)
            + (move.gives_check ? 96 : 0),
        -8192,
        8192
    );
}

[[nodiscard]] static inline int history_bonus_fp(
    const LmrMoveContext& move,
    int stat_score
) noexcept {
    if (move.quiet)
        return std::clamp(
            stat_score / QUIET_HISTORY_FP_DIVISOR,
            -3 * FP_ONE_PLY / 4,
            5 * FP_ONE_PLY / 4
        );

    return std::clamp(
        stat_score / CAPTURE_HISTORY_FP_DIVISOR,
        -FP_ONE_PLY / 2,
        FP_ONE_PLY
    );
}

[[nodiscard]] static inline int reduction_from_fp(int fp) noexcept {
    if (fp >= 0)
        return (fp + FP_ONE_PLY / 2) / FP_ONE_PLY;
    return -((-fp + FP_ONE_PLY / 2) / FP_ONE_PLY);
}

} // namespace

LmrDecision decide_lmr(const LmrNodeContext& node, const LmrMoveContext& move) noexcept {
    LmrDecision decision{};

    const bool quiet_candidate =
        move.quiet &&
        !node.pv_node &&
        !node.checked &&
        node.depth >= 3 &&
        move.move_index >= 2;
    const bool capture_candidate =
        move.simple_capture &&
        !node.pv_node &&
        !node.checked &&
        node.depth >= 4 &&
        move.reduction_index >= 2;

    if (!quiet_candidate && !capture_candidate)
        return decision;

    decision.stat_score = move.quiet
        ? quiet_stat_score(move, node.depth)
        : capture_stat_score(move);

    int fp = 0;
    fp = quiet_candidate
        ? base_quiet_reduction_fp(node.depth, move.move_index)
        : base_capture_reduction_fp(node.depth, move.reduction_index);

    // SEE-guided capture reduction: winning captures should barely be reduced.
    if (capture_candidate) {
        if (move.see_value >= 200)           fp -= FP_ONE_PLY / 2;
        else if (move.see_value >= 100)      fp -= FP_ONE_PLY / 4;
        else if (move.see_value < -50)       fp += FP_ONE_PLY / 4;
        fp = std::max(0, fp);
    }

    decision.base_reduction_fp = fp;

    if (node.improving)
        fp -= FP_ONE_PLY / 8;
    else
        fp += FP_ONE_PLY / 4;

    if (!node.tt_move_present)
        fp += FP_ONE_PLY / 2;

    if (node.cut_node)
        fp += FP_ONE_PLY / 2;
    if (node.all_node)
        fp += fp / (node.depth + 1);

    if (node.tt_move_is_capture && move.quiet)
        fp += FP_ONE_PLY / 8;

    // TT quality confidence: deeper/exact TT entries → less reduction.
    if (node.tt_move_present && node.tt_depth > 0) {
        if (node.tt_depth >= node.depth)
            fp -= FP_ONE_PLY / 4;
        if (node.tt_bound == static_cast<int>(memory::BOUND_EXACT))
            fp -= FP_ONE_PLY / 4;
        else if (node.tt_bound == static_cast<int>(memory::BOUND_LOWER))
            fp -= FP_ONE_PLY / 8;
    }

    if (move.is_tt_move)
        fp -= FP_ONE_PLY;
    if (move.gives_check)
        fp -= FP_ONE_PLY / 2;
    if (move.recapture)
        fp -= FP_ONE_PLY / 4;
    if (move.promotion)
        fp -= FP_ONE_PLY / 4;

    if (node.next_ply_cutoff_count > 1)
        fp += std::min(2, node.next_ply_cutoff_count - 1) * (FP_ONE_PLY / 4);

    if (node.parent_reduction_fp > FP_ONE_PLY)
        fp += FP_ONE_PLY / 8;

    fp -= history_bonus_fp(move, decision.stat_score);

    const int min_reduction = 0;
    const int max_reduction = move.quiet
        ? std::min(node.depth - 1, 5)
        : std::min(node.depth - 1, 3);

    decision.final_reduction_fp = std::clamp(
        fp,
        min_reduction * FP_ONE_PLY,
        max_reduction * FP_ONE_PLY
    );
    decision.final_reduction = reduction_from_fp(decision.final_reduction_fp);
    decision.eligible = decision.final_reduction > 0;
    return decision;
}

int lmr_research_depth(
    const LmrDecision& decision,
    int full_depth,
    int score,
    int alpha,
    int best_score
) noexcept {
    if (!decision.eligible)
        return full_depth;

    int research_depth = full_depth;

    if (score > alpha + LMR_DEEPER_RESEARCH_MARGIN && decision.final_reduction >= 2)
        ++research_depth;
    else if (score < alpha + LMR_SHALLOWER_RESEARCH_MARGIN)
        --research_depth;

    if (score > best_score + LMR_DEEPER_RESEARCH_MARGIN * 2 && decision.final_reduction >= 3)
        ++research_depth;

    return std::clamp(research_depth, 1, full_depth + 1);
}

} // namespace magnus::search
