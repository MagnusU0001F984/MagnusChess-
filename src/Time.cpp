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

#include "Time.h"

#include <algorithm>

#include "MoveGen.h"
#include "Nnue.h"

namespace valerain::timeman {

namespace {

[[nodiscard]] constexpr int clamp_int(int v, int lo, int hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

void TimeManager::new_game() noexcept {
    history_size_ = 0;
    next_index_ = 0;
}

std::size_t TimeManager::history_size() const noexcept {
    return history_size_;
}

bool TimeManager::build_limits(
    const Position& pos,
    const GoParams& params,
    search::SearchLimits& limits
) const noexcept {
    limits.depth = search::MAX_PLY;
    limits.node_limit = 0;
    limits.soft_time_ms = 0;
    limits.hard_time_ms = 0;
    limits.infinite = false;

    if (params.depth > 0)
        limits.depth = params.depth;

    limits.node_limit = params.nodes;
    limits.infinite = params.infinite;

    if (params.movetime > 0) {
        limits.soft_time_ms = params.movetime;
        limits.hard_time_ms = params.movetime;
        return true;
    }

    const Color side = static_cast<Color>(pos.side_to_move);
    const int remaining = side == WHITE ? params.wtime : params.btime;
    const int increment = side == WHITE ? params.winc : params.binc;

    if (!limits.infinite && remaining > 0) {
        const int move_number = std::max(1, pos.fullmove_number);
        const bool sudden_death = params.movestogo == 0 && increment == 0;

        const bool opening_very_early = move_number <= 10;
        const bool opening_early = move_number <= 20;

        int phase_scale = 100;
        if (move_number <= 10)      phase_scale = sudden_death ? 35 : 45;
        else if (move_number <= 20) phase_scale = sudden_death ? 50 : 60;
        else if (move_number <= 35) phase_scale = sudden_death ? 75 : 85;
        else if (move_number <= 50) phase_scale = sudden_death ? 100 : 105;
        else                        phase_scale = sudden_death ? 115 : 120;

        const HistoryStats stats = collect_stats(side);
        if (stats.samples > 0) {
            // 开局只保留“是否经常超预算”的轻量修正，
            // 不让 score swing / best move flip 把开局时间抬回去。
            if (stats.avg_usage_pct >= 120)      phase_scale += opening_very_early ? 6 : 12;
            else if (stats.avg_usage_pct >= 105) phase_scale += opening_very_early ? 4 : 8;
            else if (stats.avg_usage_pct <= 70)  phase_scale -= opening_very_early ? 8 : 10;
            else if (stats.avg_usage_pct <= 85)  phase_scale -= opening_very_early ? 4 : 5;

            if (!opening_early) {
                if (stats.avg_score_swing_cp >= 140)      phase_scale += 12;
                else if (stats.avg_score_swing_cp >= 90)  phase_scale += 7;
                else if (stats.avg_score_swing_cp <= 30)  phase_scale -= 3;

                if (stats.best_move_flip_pct >= 70)      phase_scale += 8;
                else if (stats.best_move_flip_pct <= 30) phase_scale -= 2;
            }
        }

        phase_scale = clamp_int(phase_scale, 35, 150);

        const int mtg =
            params.movestogo > 0 ? params.movestogo :
            sudden_death
                ? (move_number <= 10 ? 40 :
                   move_number <= 20 ? 32 :
                   move_number <= 35 ? 24 : 18)
                : (move_number <= 10 ? 30 :
                   move_number <= 20 ? 26 : 24);

        int reserve_div =
            sudden_death
                ? (move_number <= 10 ? 6 :
                   move_number <= 20 ? 8 :
                   move_number <= 35 ? 12 : 16)
                : (opening_very_early ? 10 :
                   opening_early      ? 12 :
                   phase_scale <= 75  ? 12 :
                   phase_scale <= 90  ? 16 : 24);

        if (stats.samples > 0) {
            if (stats.avg_usage_pct >= 110)
                reserve_div = std::max(4, reserve_div - (opening_early ? 1 : 2));
            else if (stats.avg_usage_pct <= 70)
                reserve_div += opening_early ? 1 : 2;
        }

        const int reserve_cap =
            sudden_death
                ? std::max(50, remaining / 3)
                : (phase_scale >= 105 ? 140 : 220);
        const int reserve_floor = sudden_death ? 50 : 10;
        const int reserve = std::clamp(remaining / std::max(1, reserve_div), reserve_floor, reserve_cap);
        const int usable = std::max(1, remaining - reserve);

        const int base = usable / std::max(1, mtg);

        int inc_share = 0;
        if (increment > 0) {
            if (opening_very_early)      inc_share = increment / 4;
            else if (opening_early)      inc_share = (increment * 3) / 10;
            else                         inc_share = (increment * (phase_scale + 20)) / 200;
        }

        int soft = (base * phase_scale) / 100 + inc_share;
        soft += usable / std::max(sudden_death ? 160 : 96, mtg * (sudden_death ? 12 : 8));

        int soft_cap =
            sudden_death
                ? (phase_scale <= 65 ? usable / 6 :
                   phase_scale <= 80 ? usable / 5 :
                   usable / 4)
                : (phase_scale <= 75 ? usable / 4 :
                   phase_scale <= 90 ? usable / 3 :
                   (usable * 2) / 5);

        // 开局更硬的上限，避免后续补偿项把预算再次抬高
        if (opening_very_early)
            soft_cap = std::min(soft_cap, sudden_death ? usable / 10 : usable / 8);
        else if (opening_early)
            soft_cap = std::min(soft_cap, sudden_death ? usable / 8 : usable / 6);

        soft = std::max(1, std::min(soft, soft_cap));

        int hard = std::min(
            usable,
            std::max(
                soft + base * (sudden_death ? 1 : 2),
                soft * (sudden_death ? 2 : (phase_scale >= 105 ? 3 : 2))
            )
        );

        // 开局 hard 不允许膨胀太离谱
        if (opening_very_early)
            hard = std::min(hard, std::max(soft, soft * 2));
        else if (opening_early)
            hard = std::min(hard, std::max(soft, (soft * 5) / 2));

        if (!opening_early &&
            stats.samples > 0 &&
            stats.avg_score_swing_cp >= 120 &&
            stats.avg_usage_pct >= 95) {
            hard = std::max(hard, std::min(usable, soft * 3));
        }

        hard = std::max(soft, hard);

        limits.soft_time_ms = soft;
        limits.hard_time_ms = hard;
    }

    if (!limits.infinite &&
        limits.depth == search::MAX_PLY &&
        limits.node_limit == 0 &&
        limits.soft_time_ms == 0 &&
        limits.hard_time_ms == 0) {
        return false;
    }

    return true;
}

void TimeManager::record_search(
    const Position& root,
    const search::SearchLimits& limits,
    const search::SearchResult& result,
    int elapsed_ms
) noexcept {
    // Only timed searches should influence the historical budget model.
    if (limits.soft_time_ms <= 0 && limits.hard_time_ms <= 0)
        return;

    SearchRecord record{};
    record.side = static_cast<Color>(root.side_to_move);
    record.fullmove_number = std::max(1, root.fullmove_number);
    record.soft_time_ms = std::max(0, limits.soft_time_ms);
    record.hard_time_ms = std::max(0, limits.hard_time_ms);
    record.elapsed_ms = std::max(0, elapsed_ms);
    record.depth = std::max(0, result.depth);
    record.score_cp =
        limits.use_nnue && nnue::loaded()
            ? nnue::search_score_to_cp(result.score, root)
            : result.score;
    record.best_move = result.best_move;

    push_record(record);
}

void TimeManager::push_record(const SearchRecord& record) noexcept {
    history_[next_index_] = record;
    next_index_ = (next_index_ + 1) % MAX_HISTORY;
    if (history_size_ < MAX_HISTORY)
        ++history_size_;
}

const TimeManager::SearchRecord& TimeManager::recent_record(
    std::size_t offset
) const noexcept {
    const std::size_t index =
        (next_index_ + MAX_HISTORY - 1 - (offset % MAX_HISTORY)) % MAX_HISTORY;
    return history_[index];
}

TimeManager::HistoryStats TimeManager::collect_stats(Color side) const noexcept {
    HistoryStats stats{};
    if (history_size_ == 0)
        return stats;

    i64 usage_sum = 0;
    int usage_count = 0;
    i64 swing_sum = 0;
    int swing_count = 0;
    int move_flip_count = 0;
    int move_transition_count = 0;

    int prev_score = 0;
    Move prev_move = 0;
    bool has_prev = false;

    constexpr std::size_t SAMPLE_CAP = 12;
    for (std::size_t i = 0; i < history_size_; ++i) {
        if (static_cast<std::size_t>(stats.samples) >= SAMPLE_CAP)
            break;

        const SearchRecord& rec = recent_record(i);
        if (rec.side != side)
            continue;

        ++stats.samples;

        if (rec.soft_time_ms > 0 && rec.elapsed_ms > 0) {
            usage_sum += (static_cast<i64>(rec.elapsed_ms) * 100) / rec.soft_time_ms;
            ++usage_count;
        }

        if (has_prev) {
            const int score_diff =
                rec.score_cp >= prev_score
                    ? rec.score_cp - prev_score
                    : prev_score - rec.score_cp;
            swing_sum += score_diff;
            ++swing_count;

            if (!move_is_none(rec.best_move) && !move_is_none(prev_move)) {
                if (rec.best_move != prev_move)
                    ++move_flip_count;
                ++move_transition_count;
            }
        }

        prev_score = rec.score_cp;
        prev_move = rec.best_move;
        has_prev = true;
    }

    if (usage_count > 0)
        stats.avg_usage_pct = static_cast<int>(usage_sum / usage_count);
    if (swing_count > 0)
        stats.avg_score_swing_cp = static_cast<int>(swing_sum / swing_count);
    if (move_transition_count > 0)
        stats.best_move_flip_pct = (move_flip_count * 100) / move_transition_count;

    return stats;
}

} // namespace valerain::timeman
