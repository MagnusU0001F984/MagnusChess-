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

#include "See.h"

#include <bit>

#include "Attack.h"

namespace valerain::search {

namespace {

constexpr int see_piece_value[PIECE_TYPE_NB] = {
    100, 320, 330, 500, 900, 20000
};

} // namespace

bool see_ge(
    const Position& pos,
    const memory::Memory& mem,
    Move move,
    int threshold
) noexcept {
    if (!move_is_capture(move))
        return threshold <= 0;

    const Color us = static_cast<Color>(pos.side_to_move);
    const Square from = from_sq(move);
    const Square to = to_sq(move);
    const PieceType moving = piece_type_on(pos, from);
    if (!is_ok(moving))
        return false;

    const PieceType captured = move_is_ep(move)
        ? PAWN
        : piece_type_on(pos, to);
    if (!is_ok(captured))
        return false;

    int balance = see_piece_value[captured] - threshold;
    PieceType next_victim = moving;

    if (move_is_promotion(move)) {
        const PieceType promo = promo_piece(move);
        if (!is_ok(promo))
            return false;
        balance += see_piece_value[promo] - see_piece_value[PAWN];
        next_victim = promo;
    }

    if (balance < 0)
        return false;

    balance -= see_piece_value[next_victim];
    if (balance >= 0)
        return true;

    Bitboard occupied = pos.occupied ^ bb_of(from);
    if (move_is_ep(move)) {
        const Square cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
        occupied ^= bb_of(cap_sq);
    } else {
        occupied ^= bb_of(to);
    }

    const Bitboard bishop_like = pos.piece_bb[BISHOP] | pos.piece_bb[QUEEN];
    const Bitboard rook_like = pos.piece_bb[ROOK] | pos.piece_bb[QUEEN];
    Bitboard attackers = attackers_to(pos, mem, to, occupied);

    Color side = static_cast<Color>(us ^ 1);
    while (true) {
        attackers &= occupied;
        const Bitboard side_attackers = attackers & pos.color_bb[side];
        if (side_attackers == 0ULL)
            break;

        PieceType attacker = PIECE_TYPE_NONE;
        Bitboard from_set = 0ULL;

        Bitboard by_pt = side_attackers & pos.piece_bb[PAWN];
        if (by_pt) {
            attacker = PAWN;
            from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
        } else {
            by_pt = side_attackers & pos.piece_bb[KNIGHT];
            if (by_pt) {
                attacker = KNIGHT;
                from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
            } else {
                by_pt = side_attackers & pos.piece_bb[BISHOP];
                if (by_pt) {
                    attacker = BISHOP;
                    from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                } else {
                    by_pt = side_attackers & pos.piece_bb[ROOK];
                    if (by_pt) {
                        attacker = ROOK;
                        from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                    } else {
                        by_pt = side_attackers & pos.piece_bb[QUEEN];
                        if (by_pt) {
                            attacker = QUEEN;
                            from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                        } else {
                            by_pt = side_attackers & pos.piece_bb[KING];
                            if (by_pt) {
                                attacker = KING;
                                from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                            }
                        }
                    }
                }
            }
        }

        if (attacker == PIECE_TYPE_NONE)
            break;

        occupied ^= from_set;

        if (attacker == PAWN || attacker == BISHOP || attacker == QUEEN)
            attackers |= bishop_attacks(mem, to, occupied) & bishop_like;
        if (attacker == ROOK || attacker == QUEEN)
            attackers |= rook_attacks(mem, to, occupied) & rook_like;

        attackers &= occupied;
        balance = see_piece_value[attacker] - balance;
        side = static_cast<Color>(side ^ 1);

        if (balance >= 0) {
            if (attacker == KING && (attackers & pos.color_bb[side]) != 0ULL)
                side = static_cast<Color>(side ^ 1);
            break;
        }
    }

    return side != us;
}

} // namespace valerain::search
