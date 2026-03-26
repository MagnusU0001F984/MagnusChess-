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

#include "History.h"

#include <cstring>

namespace valerain::search {

namespace {

constexpr i32 HISTORY_LIMIT = 32767;

[[nodiscard]] inline bool valid_ply(int ply) noexcept {
    return ply >= 0 && ply < MAX_PLY;
}

} // namespace

void HistoryTable::clear() noexcept {
    std::memset(killers, 0, sizeof(killers));
    std::memset(quiet, 0, sizeof(quiet));
}

Move HistoryTable::killer(int ply, int slot) const noexcept {
    if (!valid_ply(ply) || slot < 0 || slot > 1)
        return Move(0);

    return killers[ply][slot];
}

i32 HistoryTable::quiet_value(const Position& pos, Move move) const noexcept {
    if (move_is_capture(move))
        return 0;

    const Color side = static_cast<Color>(pos.side_to_move);
    const PieceType pt = piece_type_on(pos, from_sq(move));
    if (!is_ok(side) || !is_ok(pt))
        return 0;

    return quiet[side][pt][to_sq(move)];
}

void HistoryTable::bonus(const Position& pos, Move move, int depth) noexcept {
    if (move_is_capture(move))
        return;

    const Color side = static_cast<Color>(pos.side_to_move);
    const PieceType pt = piece_type_on(pos, from_sq(move));
    if (!is_ok(side) || !is_ok(pt))
        return;

    i32& h = quiet[side][pt][to_sq(move)];
    h += static_cast<i32>(depth * depth);
    if (h > HISTORY_LIMIT)
        h = HISTORY_LIMIT;
}

void HistoryTable::penalty(const Position& pos, Move move, int depth) noexcept {
    if (move_is_capture(move))
        return;

    const Color side = static_cast<Color>(pos.side_to_move);
    const PieceType pt = piece_type_on(pos, from_sq(move));
    if (!is_ok(side) || !is_ok(pt))
        return;

    i32& h = quiet[side][pt][to_sq(move)];
    h -= static_cast<i32>(depth * depth * 4);
    if (h < -HISTORY_LIMIT)
        h = -HISTORY_LIMIT;
}

void HistoryTable::penalize_quiets(
    const Position& pos,
    const Move* quiets,
    int count,
    Move excluded_move,
    int depth
) noexcept {
    for (int i = 0; i < count; ++i) {
        if (quiets[i] == excluded_move)
            continue;
        penalty(pos, quiets[i], depth);
    }
}

void HistoryTable::reward_cutoff(
    const Position& pos,
    Move move,
    int depth,
    int ply
) noexcept {
    if (move_is_capture(move))
        return;

    if (valid_ply(ply) && killers[ply][0] != move) {
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = move;
    }

    bonus(pos, move, depth);
}

} // namespace valerain::search
