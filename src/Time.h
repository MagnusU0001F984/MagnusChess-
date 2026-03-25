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

#include <array>
#include <cstddef>

#include "Position.h"
#include "Search.h"
#include "Types.h"

namespace valerain::timeman {

/*
Normalized go parameters extracted from UCI. TimeManager converts these into
search::SearchLimits and adapts the budget with recent-search history.
*/
struct GoParams {
    int depth = 0;
    u64 nodes = 0;
    int movetime = 0;
    int wtime = 0;
    int btime = 0;
    int winc = 0;
    int binc = 0;
    int movestogo = 0;
    bool infinite = false;
};

class TimeManager {
public:
    TimeManager() = default;

    void new_game() noexcept;

    [[nodiscard]] bool build_limits(
        const Position& pos,
        const GoParams& params,
        search::SearchLimits& limits
    ) const noexcept;

    void record_search(
        const Position& root,
        const search::SearchLimits& limits,
        const search::SearchResult& result,
        int elapsed_ms
    ) noexcept;

    [[nodiscard]] std::size_t history_size() const noexcept;

private:
    struct SearchRecord {
        Color side = WHITE;
        int fullmove_number = 1;
        int soft_time_ms = 0;
        int hard_time_ms = 0;
        int elapsed_ms = 0;
        int depth = 0;
        int score_cp = 0;
        Move best_move = 0;
    };

    struct HistoryStats {
        int samples = 0;
        int avg_usage_pct = 100;
        int avg_score_swing_cp = 0;
        int best_move_flip_pct = 50;
    };

    static constexpr std::size_t MAX_HISTORY = 64;

    std::array<SearchRecord, MAX_HISTORY> history_{};
    std::size_t history_size_ = 0;
    std::size_t next_index_ = 0;

    void push_record(const SearchRecord& record) noexcept;
    [[nodiscard]] HistoryStats collect_stats(Color side) const noexcept;
    [[nodiscard]] const SearchRecord& recent_record(std::size_t offset) const noexcept;
};

} // namespace valerain::timeman

