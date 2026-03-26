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

#include "MoveGen.h"
#include "Position.h"
#include "Search.h"
#include "Types.h"

namespace valerain::search {

/*
HistoryTable stores killer moves and quiet-move history used for move ordering
and pruning decisions in the main search.
*/
struct HistoryTable {
    Move killers[MAX_PLY][2]{};
    i32 quiet[COLOR_NB][PIECE_TYPE_NB][SQ_NB]{};

    void clear() noexcept;

    [[nodiscard]] Move killer(int ply, int slot) const noexcept;
    [[nodiscard]] i32 quiet_value(const Position& pos, Move move) const noexcept;

    void bonus(const Position& pos, Move move, int depth) noexcept;
    void penalty(const Position& pos, Move move, int depth) noexcept;

    void penalize_quiets(
        const Position& pos,
        const Move* quiets,
        int count,
        Move excluded_move,
        int depth
    ) noexcept;

    void reward_cutoff(
        const Position& pos,
        Move move,
        int depth,
        int ply
    ) noexcept;
};

} // namespace valerain::search
