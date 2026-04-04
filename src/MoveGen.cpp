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

#include "MoveGen.h"
#include "Attack.h"
#include "Position.h"

#include <bit>

/*
This file builds legal move lists from bitboards plus precomputed attack tables.
It first derives side-to-move context (checkers, pins, masks),
then uses specialized generators for evasions, non-evasions, and the final
legality filter.
*/

namespace valerain {

namespace {

inline Color opposite(Color c) noexcept {
    return c == WHITE ? BLACK : WHITE;
}

inline Square lsb_sq(Bitboard bb) noexcept {
    return static_cast<Square>(std::countr_zero(bb));
}

inline bool more_than_one(Bitboard bb) noexcept {
    return (bb & (bb - 1)) != 0;
}

inline Bitboard pieces_bb(const Position& pos, Color c, PieceType pt) noexcept {
    return pos.color_bb[c] & pos.piece_bb[pt];
}

inline Bitboard diag_sliders_bb(const Position& pos, Color c) noexcept {
    return pieces_bb(pos, c, BISHOP) | pieces_bb(pos, c, QUEEN);
}

inline Bitboard ortho_sliders_bb(const Position& pos, Color c) noexcept {
    return pieces_bb(pos, c, ROOK) | pieces_bb(pos, c, QUEEN);
}

inline Bitboard attacks_by_color_on_occ(
    const Position& pos,
    const memory::Memory& mem,
    Color by,
    Bitboard occupied
) noexcept {
    Bitboard attacks = 0ULL;

    Bitboard pawns   = pieces_bb(pos, by, PAWN);
    Bitboard knights = pieces_bb(pos, by, KNIGHT);
    Bitboard bishops = pieces_bb(pos, by, BISHOP);
    Bitboard rooks   = pieces_bb(pos, by, ROOK);
    Bitboard queens  = pieces_bb(pos, by, QUEEN);
    Bitboard kings   = pieces_bb(pos, by, KING);

    while (pawns) {
        const Square sq = lsb_sq(pawns);
        pawns &= pawns - 1;
        attacks |= pawn_attacks(mem, by, sq);
    }
    while (knights) {
        const Square sq = lsb_sq(knights);
        knights &= knights - 1;
        attacks |= knight_attacks(mem, sq);
    }
    while (bishops) {
        const Square sq = lsb_sq(bishops);
        bishops &= bishops - 1;
        attacks |= bishop_attacks(mem, sq, occupied);
    }
    while (rooks) {
        const Square sq = lsb_sq(rooks);
        rooks &= rooks - 1;
        attacks |= rook_attacks(mem, sq, occupied);
    }
    while (queens) {
        const Square sq = lsb_sq(queens);
        queens &= queens - 1;
        attacks |= queen_attacks(mem, sq, occupied);
    }
    while (kings) {
        const Square sq = lsb_sq(kings);
        kings &= kings - 1;
        attacks |= king_attacks(mem, sq);
    }

    return attacks;
}

inline Move* append_moves_from_mask(
    Square from,
    Bitboard mask,
    Bitboard them_occ,
    Move* out
) noexcept {
    while (mask) {
        const Square to = lsb_sq(mask);
        mask &= mask - 1;

        const u16 flag = (them_occ & bb_of(to)) ? MOVE_CAPTURE : MOVE_QUIET;
        *out++ = make_move(from, to, flag);
    }

    return out;
}

inline Move* append_promotion_moves(
    Square from,
    Square to,
    bool is_capture,
    Move* out
) noexcept {
    if (is_capture) {
        *out++ = make_move(from, to, MOVE_CAP_PROMO_N);
        *out++ = make_move(from, to, MOVE_CAP_PROMO_B);
        *out++ = make_move(from, to, MOVE_CAP_PROMO_R);
        *out++ = make_move(from, to, MOVE_CAP_PROMO_Q);
    } else {
        *out++ = make_move(from, to, MOVE_PROMO_N);
        *out++ = make_move(from, to, MOVE_PROMO_B);
        *out++ = make_move(from, to, MOVE_PROMO_R);
        *out++ = make_move(from, to, MOVE_PROMO_Q);
    }
    return out;
}

inline Bitboard pin_mask_for(
    const memory::Memory& mem,
    const GenInfo& info,
    Square from
) noexcept {
    const Bitboard from_bb = bb_of(from);
    if ((info.pinned & from_bb) == 0ULL)
        return ~0ULL;

    // A pinned piece may move anywhere on the full king-pinner ray, including
    // captures of the pinner beyond the pinned piece itself.
    Bitboard pinners = info.pinners;
    while (pinners) {
        const Square pinner_sq = lsb_sq(pinners);
        pinners &= pinners - 1;

        if ((between_bb(mem, info.king_sq, pinner_sq) & from_bb) != 0ULL)
            return line_bb(mem, info.king_sq, pinner_sq);
    }

    return line_bb(mem, info.king_sq, from);
}

inline Move* generate_knight_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard knights = pieces_bb(pos, info.us, KNIGHT);
    const Bitboard target = info.capture_mask | info.push_mask;

    while (knights) {
        const Square from = lsb_sq(knights);
        knights &= knights - 1;

        Bitboard mask = knight_attacks(mem, from);
        mask &= ~info.us_occ;
        mask &= target;
        mask &= pin_mask_for(mem, info, from);

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_bishop_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard bishops = pieces_bb(pos, info.us, BISHOP);
    const Bitboard target = info.capture_mask | info.push_mask;

    while (bishops) {
        const Square from = lsb_sq(bishops);
        bishops &= bishops - 1;

        Bitboard mask = bishop_attacks(mem, from, info.occupied);
        mask &= ~info.us_occ;
        mask &= target;
        mask &= pin_mask_for(mem, info, from);

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_rook_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard rooks = pieces_bb(pos, info.us, ROOK);
    const Bitboard target = info.capture_mask | info.push_mask;

    while (rooks) {
        const Square from = lsb_sq(rooks);
        rooks &= rooks - 1;

        Bitboard mask = rook_attacks(mem, from, info.occupied);
        mask &= ~info.us_occ;
        mask &= target;
        mask &= pin_mask_for(mem, info, from);

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_queen_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard queens = pieces_bb(pos, info.us, QUEEN);
    const Bitboard target = info.capture_mask | info.push_mask;

    while (queens) {
        const Square from = lsb_sq(queens);
        queens &= queens - 1;

        Bitboard mask = queen_attacks(mem, from, info.occupied);
        mask &= ~info.us_occ;
        mask &= target;
        mask &= pin_mask_for(mem, info, from);

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_white_pawn_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    // In check, pawn moves are limited to captures of the checker or pushes that block the ray.
    Bitboard pawns = pieces_bb(pos, WHITE, PAWN);

    while (pawns) {
        const Square from = lsb_sq(pawns);
        pawns &= pawns - 1;

        const int r = rank_of(from);
        const int f = file_of(from);
        const Bitboard pin_mask = pin_mask_for(mem, info, from);

        const Square one = from + 8;
        if (one < SQ_NB && !(info.occupied & bb_of(one))) {
            if ((info.push_mask & bb_of(one)) && (pin_mask & bb_of(one))) {
                if (r == 6) out = append_promotion_moves(from, one, false, out);
                else        *out++ = make_move(from, one, MOVE_QUIET);
            }

            if (r == 1) {
                const Square two = from + 16;
                if (!(info.occupied & bb_of(two)) &&
                    (info.push_mask & bb_of(two)) &&
                    (pin_mask & bb_of(two))) {
                    *out++ = make_move(from, two, MOVE_DOUBLE_PUSH);
                }
            }
        }

        if (f > 0) {
            const Square to = from + 7;
            if ((info.capture_mask & bb_of(to)) &&
                (info.them_occ & bb_of(to)) &&
                (pin_mask & bb_of(to))) {
                if (r == 6) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }

        if (f < 7) {
            const Square to = from + 9;
            if ((info.capture_mask & bb_of(to)) &&
                (info.them_occ & bb_of(to)) &&
                (pin_mask & bb_of(to))) {
                if (r == 6) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }
    }

    return out;
}

inline Move* generate_black_pawn_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard pawns = pieces_bb(pos, BLACK, PAWN);

    while (pawns) {
        const Square from = lsb_sq(pawns);
        pawns &= pawns - 1;

        const int r = rank_of(from);
        const int f = file_of(from);
        const Bitboard pin_mask = pin_mask_for(mem, info, from);

        const Square one = from - 8;
        if (one >= 0 && !(info.occupied & bb_of(one))) {
            if ((info.push_mask & bb_of(one)) && (pin_mask & bb_of(one))) {
                if (r == 1) out = append_promotion_moves(from, one, false, out);
                else        *out++ = make_move(from, one, MOVE_QUIET);
            }

            if (r == 6) {
                const Square two = from - 16;
                if (!(info.occupied & bb_of(two)) &&
                    (info.push_mask & bb_of(two)) &&
                    (pin_mask & bb_of(two))) {
                    *out++ = make_move(from, two, MOVE_DOUBLE_PUSH);
                }
            }
        }

        if (f > 0) {
            const Square to = from - 9;
            if ((info.capture_mask & bb_of(to)) &&
                (info.them_occ & bb_of(to)) &&
                (pin_mask & bb_of(to))) {
                if (r == 1) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }

        if (f < 7) {
            const Square to = from - 7;
            if ((info.capture_mask & bb_of(to)) &&
                (info.them_occ & bb_of(to)) &&
                (pin_mask & bb_of(to))) {
                if (r == 1) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }
    }

    return out;
}

inline Move* generate_pawn_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    return info.us == WHITE
        ? generate_white_pawn_evasions(pos, mem, info, out)
        : generate_black_pawn_evasions(pos, mem, info, out);
}

inline Move* generate_king_non_evasions(
    const Position&,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    const Square from = info.king_sq;
    Bitboard mask = king_attacks(mem, from);
    mask &= ~info.us_occ;
    mask &= ~info.danger;

    return append_moves_from_mask(from, mask, info.them_occ, out);
}

inline Move* generate_knight_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard knights = pieces_bb(pos, info.us, KNIGHT);

    while (knights) {
        const Square from = lsb_sq(knights);
        knights &= knights - 1;

        Bitboard mask = knight_attacks(mem, from);
        mask &= ~info.us_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_bishop_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard bishops = pieces_bb(pos, info.us, BISHOP);

    while (bishops) {
        const Square from = lsb_sq(bishops);
        bishops &= bishops - 1;

        Bitboard mask = bishop_attacks(mem, from, info.occupied);
        mask &= ~info.us_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_rook_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard rooks = pieces_bb(pos, info.us, ROOK);

    while (rooks) {
        const Square from = lsb_sq(rooks);
        rooks &= rooks - 1;

        Bitboard mask = rook_attacks(mem, from, info.occupied);
        mask &= ~info.us_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_queen_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard queens = pieces_bb(pos, info.us, QUEEN);

    while (queens) {
        const Square from = lsb_sq(queens);
        queens &= queens - 1;

        Bitboard mask = queen_attacks(mem, from, info.occupied);
        mask &= ~info.us_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_white_pawn_non_evasions(
    const Position& pos,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard pawns = pieces_bb(pos, WHITE, PAWN);

    while (pawns) {
        const Square from = lsb_sq(pawns);
        pawns &= pawns - 1;

        const int r = rank_of(from);
        const int f = file_of(from);

        const Square one = from + 8;
        if (one < SQ_NB && !(info.occupied & bb_of(one))) {
            if (r == 6) {
                out = append_promotion_moves(from, one, false, out);
            } else {
                *out++ = make_move(from, one, MOVE_QUIET);

                if (r == 1) {
                    const Square two = from + 16;
                    if (!(info.occupied & bb_of(two)))
                        *out++ = make_move(from, two, MOVE_DOUBLE_PUSH);
                }
            }
        }

        if (f > 0) {
            const Square to = from + 7;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 6) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }

        if (f < 7) {
            const Square to = from + 9;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 6) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }
    }

    return out;
}

inline Move* generate_black_pawn_non_evasions(
    const Position& pos,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard pawns = pieces_bb(pos, BLACK, PAWN);

    while (pawns) {
        const Square from = lsb_sq(pawns);
        pawns &= pawns - 1;

        const int r = rank_of(from);
        const int f = file_of(from);

        const Square one = from - 8;
        if (one >= 0 && !(info.occupied & bb_of(one))) {
            if (r == 1) {
                out = append_promotion_moves(from, one, false, out);
            } else {
                *out++ = make_move(from, one, MOVE_QUIET);

                if (r == 6) {
                    const Square two = from - 16;
                    if (!(info.occupied & bb_of(two)))
                        *out++ = make_move(from, two, MOVE_DOUBLE_PUSH);
                }
            }
        }

        if (f > 0) {
            const Square to = from - 9;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 1) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }

        if (f < 7) {
            const Square to = from - 7;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 1) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }
    }

    return out;
}

inline Move* generate_pawn_non_evasions(
    const Position& pos,
    const GenInfo& info,
    Move* out
) noexcept {
    return info.us == WHITE
        ? generate_white_pawn_non_evasions(pos, info, out)
        : generate_black_pawn_non_evasions(pos, info, out);
}

inline Move* generate_king_captures(
    const Position&,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    const Square from = info.king_sq;
    Bitboard mask = king_attacks(mem, from);
    mask &= info.them_occ;
    mask &= ~info.danger;
    return append_moves_from_mask(from, mask, info.them_occ, out);
}

inline Move* generate_knight_captures(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard knights = pieces_bb(pos, info.us, KNIGHT);

    while (knights) {
        const Square from = lsb_sq(knights);
        knights &= knights - 1;

        Bitboard mask = knight_attacks(mem, from);
        mask &= info.them_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_bishop_captures(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard bishops = pieces_bb(pos, info.us, BISHOP);

    while (bishops) {
        const Square from = lsb_sq(bishops);
        bishops &= bishops - 1;

        Bitboard mask = bishop_attacks(mem, from, info.occupied);
        mask &= info.them_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_rook_captures(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard rooks = pieces_bb(pos, info.us, ROOK);

    while (rooks) {
        const Square from = lsb_sq(rooks);
        rooks &= rooks - 1;

        Bitboard mask = rook_attacks(mem, from, info.occupied);
        mask &= info.them_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_queen_captures(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard queens = pieces_bb(pos, info.us, QUEEN);

    while (queens) {
        const Square from = lsb_sq(queens);
        queens &= queens - 1;

        Bitboard mask = queen_attacks(mem, from, info.occupied);
        mask &= info.them_occ;

        out = append_moves_from_mask(from, mask, info.them_occ, out);
    }

    return out;
}

inline Move* generate_white_pawn_captures(
    const Position& pos,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard pawns = pieces_bb(pos, WHITE, PAWN);

    while (pawns) {
        const Square from = lsb_sq(pawns);
        pawns &= pawns - 1;

        const int r = rank_of(from);
        const int f = file_of(from);

        if (f > 0) {
            const Square to = from + 7;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 6) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }

        if (f < 7) {
            const Square to = from + 9;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 6) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }
    }

    return out;
}

inline Move* generate_black_pawn_captures(
    const Position& pos,
    const GenInfo& info,
    Move* out
) noexcept {
    Bitboard pawns = pieces_bb(pos, BLACK, PAWN);

    while (pawns) {
        const Square from = lsb_sq(pawns);
        pawns &= pawns - 1;

        const int r = rank_of(from);
        const int f = file_of(from);

        if (f > 0) {
            const Square to = from - 9;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 1) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }

        if (f < 7) {
            const Square to = from - 7;
            if ((info.them_occ & bb_of(to)) != 0ULL) {
                if (r == 1) out = append_promotion_moves(from, to, true, out);
                else        *out++ = make_move(from, to, MOVE_CAPTURE);
            }
        }
    }

    return out;
}

inline Move* generate_pawn_captures(
    const Position& pos,
    const GenInfo& info,
    Move* out
) noexcept {
    return info.us == WHITE
        ? generate_white_pawn_captures(pos, info, out)
        : generate_black_pawn_captures(pos, info, out);
}

inline Move* generate_ep_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    if (info.ep_sq == NO_SQ)
        return out;

    const Bitboard pawns = pieces_bb(pos, info.us, PAWN);
    Bitboard ep_attackers = pawn_attacks(mem, opposite(info.us), info.ep_sq) & pawns;

    while (ep_attackers) {
        const Square from = lsb_sq(ep_attackers);
        ep_attackers &= ep_attackers - 1;

        // 普通 pin line 过滤先保留；
        // 但 EP 真正是否合法，后面还要靠 legal()/copy-make 再判一次。
        if (!(pin_mask_for(mem, info, from) & bb_of(info.ep_sq)))
            continue;

        *out++ = make_move(from, info.ep_sq, MOVE_EP);
    }

    return out;
}

inline Move* generate_capture_non_evasions_with_info(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    out = generate_king_captures(pos, mem, info, out);
    out = generate_knight_captures(pos, mem, info, out);
    out = generate_bishop_captures(pos, mem, info, out);
    out = generate_rook_captures(pos, mem, info, out);
    out = generate_queen_captures(pos, mem, info, out);
    out = generate_pawn_captures(pos, info, out);
    out = generate_ep_non_evasions(pos, mem, info, out);
    return out;
}

inline Move* generate_ep_evasions(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    if (info.ep_sq == NO_SQ || info.double_check)
        return out;

    // EP 的目标格是 ep_sq，但被吃掉的兵在 cap_sq
    const Square cap_sq = (info.us == WHITE) ? (info.ep_sq - 8) : (info.ep_sq + 8);
    const Bitboard ep_to_bb = bb_of(info.ep_sq);
    const Bitboard cap_bb   = bb_of(cap_sq);

    // EP 能解将的两种情况：
    // 1) 吃掉的那只兵本身就是 checker
    // 2) 落到 ep_sq 这个格正好是 block square
    if (!(info.capture_mask & cap_bb) && !(info.push_mask & ep_to_bb))
        return out;

    const Bitboard pawns = pieces_bb(pos, info.us, PAWN);
    Bitboard ep_attackers = pawn_attacks(mem, opposite(info.us), info.ep_sq) & pawns;

    while (ep_attackers) {
        const Square from = lsb_sq(ep_attackers);
        ep_attackers &= ep_attackers - 1;

        if (!(pin_mask_for(mem, info, from) & ep_to_bb))
            continue;

        *out++ = make_move(from, info.ep_sq, MOVE_EP);
    }

    return out;
}

inline Move* generate_castling_non_evasions(
    const Position& pos,
    const memory::Memory&,
    const GenInfo& info,
    Move* out
) noexcept {
    // 王在将军中绝不能易位
    if (info.in_check)
        return out;

    if (info.us == WHITE) {
        // White O-O: e1 -> g1, rook h1 -> f1
        if ((pos.castling_rights & WHITE_OO) &&
            piece_on(pos, 4) == W_KING &&
            piece_on(pos, 7) == W_ROOK &&
            !(info.occupied & (bb_of(5) | bb_of(6))) &&
            !(info.danger & (bb_of(5) | bb_of(6)))) {
            *out++ = make_move(4, 6, MOVE_OO);
        }

        // White O-O-O: e1 -> c1, rook a1 -> d1
        if ((pos.castling_rights & WHITE_OOO) &&
            piece_on(pos, 4) == W_KING &&
            piece_on(pos, 0) == W_ROOK &&
            !(info.occupied & (bb_of(1) | bb_of(2) | bb_of(3))) &&
            !(info.danger & (bb_of(2) | bb_of(3)))) {
            *out++ = make_move(4, 2, MOVE_OOO);
        }
    } else {
        // Black O-O: e8 -> g8, rook h8 -> f8
        if ((pos.castling_rights & BLACK_OO) &&
            piece_on(pos, 60) == B_KING &&
            piece_on(pos, 63) == B_ROOK &&
            !(info.occupied & (bb_of(61) | bb_of(62))) &&
            !(info.danger & (bb_of(61) | bb_of(62)))) {
            *out++ = make_move(60, 62, MOVE_OO);
        }

        // Black O-O-O: e8 -> c8, rook a8 -> d8
        if ((pos.castling_rights & BLACK_OOO) &&
            piece_on(pos, 60) == B_KING &&
            piece_on(pos, 56) == B_ROOK &&
            !(info.occupied & (bb_of(57) | bb_of(58) | bb_of(59))) &&
            !(info.danger & (bb_of(58) | bb_of(59)))) {
            *out++ = make_move(60, 58, MOVE_OOO);
        }
    }

    return out;
}

inline void compute_pinners_and_pinned(
    const Position& pos,
    const memory::Memory& mem,
    Color us,
    Bitboard& pinners,
    Bitboard& pinned
) noexcept {
    const Color them = opposite(us);
    const Square ksq = pos.king_sq[us];
    const Bitboard own = pos.color_bb[us];

    pinners = 0ULL;
    pinners |= rook_xray_attacks(mem, ksq, pos.occupied, own) & ortho_sliders_bb(pos, them);
    pinners |= bishop_xray_attacks(mem, ksq, pos.occupied, own) & diag_sliders_bb(pos, them);

    pinned = 0ULL;
    Bitboard tmp = pinners;
    while (tmp) {
        const Square psq = lsb_sq(tmp);
        tmp &= tmp - 1;
        pinned |= between_bb(mem, ksq, psq) & own;
    }
}

inline bool legal_slow(
    Position& pos,
    const memory::Memory& mem,
    Move m
) noexcept {
    if (move_is_none(m))
        return false;

    const Color us   = static_cast<Color>(pos.side_to_move);
    const Color them = (us == WHITE ? BLACK : WHITE);

    StateInfo st{};
    make_move(pos, m, mem.tables, st);

    const Square ksq = pos.king_sq[us];
    const bool legal = attackers_to_color(pos, mem, ksq, them, pos.occupied) == 0ULL;

    unmake_move(pos, m, mem.tables, st);
    return legal;
}

inline bool legal_fast_impl(
    Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move m
) noexcept {
    if (move_is_none(m))
        return false;

    const Square from = from_sq(m);
    const Piece moving = piece_on(pos, from);
    if (moving == PIECE_NONE)
        return false;

    if (type_of(moving) == KING)
        return true;

    if (move_is_ep(m))
        return legal_slow(pos, mem, m);

    if ((info.pinned & bb_of(from)) == 0ULL)
        return true;

    return legal_slow(pos, mem, m);
}

inline Move* generate_non_evasions_with_info(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    out = generate_king_non_evasions(pos, mem, info, out);
    out = generate_knight_non_evasions(pos, mem, info, out);
    out = generate_bishop_non_evasions(pos, mem, info, out);
    out = generate_rook_non_evasions(pos, mem, info, out);
    out = generate_queen_non_evasions(pos, mem, info, out);
    out = generate_pawn_non_evasions(pos, info, out);
    out = generate_ep_non_evasions(pos, mem, info, out);
    out = generate_castling_non_evasions(pos, mem, info, out);
    return out;
}

inline Move* generate_evasions_with_info(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    out = generate_king_non_evasions(pos, mem, info, out);

    if (info.double_check)
        return out;

    out = generate_knight_evasions(pos, mem, info, out);
    out = generate_bishop_evasions(pos, mem, info, out);
    out = generate_rook_evasions(pos, mem, info, out);
    out = generate_queen_evasions(pos, mem, info, out);
    out = generate_pawn_evasions(pos, mem, info, out);
    out = generate_ep_evasions(pos, mem, info, out);
    return out;
}

inline Move* generate_pseudo_legal_with_info(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    return info.in_check
        ? generate_evasions_with_info(pos, mem, info, out)
        : generate_non_evasions_with_info(pos, mem, info, out);
}

} // namespace

Bitboard attackers_to_color(
    const Position& pos,
    const memory::Memory& mem,
    Square sq,
    Color by,
    Bitboard occupied
) noexcept {
    Bitboard attackers = 0ULL;

    const Bitboard pawns   = pieces_bb(pos, by, PAWN);
    const Bitboard knights = pieces_bb(pos, by, KNIGHT);
    const Bitboard bishops = pieces_bb(pos, by, BISHOP);
    const Bitboard rooks   = pieces_bb(pos, by, ROOK);
    const Bitboard queens  = pieces_bb(pos, by, QUEEN);
    const Bitboard kings   = pieces_bb(pos, by, KING);

    attackers |= pawn_attacks(mem, opposite(by), sq) & pawns;
    attackers |= knight_attacks(mem, sq) & knights;
    attackers |= king_attacks(mem, sq) & kings;
    attackers |= bishop_attacks(mem, sq, occupied) & (bishops | queens);
    attackers |= rook_attacks(mem, sq, occupied) & (rooks | queens);

    return attackers;
}

Bitboard attackers_to(
    const Position& pos,
    const memory::Memory& mem,
    Square sq,
    Bitboard occupied
) noexcept {
    return attackers_to_color(pos, mem, sq, WHITE, occupied)
         | attackers_to_color(pos, mem, sq, BLACK, occupied);
}

Bitboard checkers_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color us
) noexcept {
    // Squares attacking our king determine whether we are in check and how
    // evasions must be constrained.
    const Color them = opposite(us);
    return attackers_to_color(pos, mem, pos.king_sq[us], them, pos.occupied);
}

Bitboard pinners_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color us
) noexcept {
    Bitboard pinners = 0ULL;
    Bitboard pinned = 0ULL;
    compute_pinners_and_pinned(pos, mem, us, pinners, pinned);
    return pinners;
}

Bitboard pinned_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color us
) noexcept {
    Bitboard pinners = 0ULL;
    Bitboard pinned = 0ULL;
    compute_pinners_and_pinned(pos, mem, us, pinners, pinned);
    return pinned;
}

Bitboard danger_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color by
) noexcept {
    const Color us = opposite(by);
    const Bitboard occupied = pos.occupied ^ bb_of(pos.king_sq[us]);
    return attacks_by_color_on_occ(pos, mem, by, occupied);
}

void init_gen_info(
    GenInfo& info,
    const Position& pos,
    const memory::Memory& mem
) noexcept {
    info.us   = static_cast<Color>(pos.side_to_move);
    info.them = opposite(info.us);

    info.king_sq = pos.king_sq[info.us];
    info.ep_sq   = pos.ep_sq;

    info.occupied = pos.occupied;
    info.empty    = ~info.occupied;
    info.us_occ   = pos.color_bb[info.us];
    info.them_occ = pos.color_bb[info.them];

    info.checkers = checkers_bb(pos, mem, info.us);
    compute_pinners_and_pinned(pos, mem, info.us, info.pinners, info.pinned);
    info.danger = danger_bb(pos, mem, info.them);

    info.in_check     = info.checkers != 0;
    info.double_check = more_than_one(info.checkers);

    if (!info.in_check) {
        info.capture_mask = info.them_occ;
        info.push_mask    = ~info.occupied;
        return;
    }

    if (info.double_check) {
        info.capture_mask = 0ULL;
        info.push_mask    = 0ULL;
        return;
    }

    const Square checker_sq = lsb_sq(info.checkers);
    const Bitboard checker_mask = bb_of(checker_sq);

    const Bitboard slider_checkers =
        diag_sliders_bb(pos, info.them) | ortho_sliders_bb(pos, info.them);

    info.capture_mask = checker_mask;
    info.push_mask =
        (checker_mask & slider_checkers)
            ? between_bb(mem, info.king_sq, checker_sq)
            : 0ULL;
}

bool pseudo_legal(
    const Position& pos,
    const memory::Memory& mem,
    Move m
) noexcept {
    Move pseudo[MAX_MOVES];
    Move* end = generate_pseudo_legal(pos, mem, pseudo);

    for (Move* it = pseudo; it != end; ++it) {
        if (*it == m)
            return true;
    }

    return false;
}

bool legal(
    Position& pos,
    const memory::Memory& mem,
    Move m
) noexcept {
    return legal_slow(pos, mem, m);
}

bool legal(
    const Position& pos,
    const memory::Memory& mem,
    Move m
) noexcept {
    Position work = pos;
    return legal(work, mem, m);
}

bool legal_fast(
    Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move m
) noexcept {
    return legal_fast_impl(pos, mem, info, m);
}

Move* generate_captures(
    Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    // In non-check nodes, avoid full pseudo generation and build capture
    // candidates directly for qsearch.
    GenInfo info{};
    init_gen_info(info, pos, mem);

    Move pseudo[MAX_MOVES];
    Move* mid = info.in_check
        ? generate_evasions_with_info(pos, mem, info, pseudo)
        : generate_capture_non_evasions_with_info(pos, mem, info, pseudo);

    for (Move* it = pseudo; it != mid; ++it) {
        if (!move_is_capture(*it))
            continue;
        if (legal_fast_impl(pos, mem, info, *it))
            *out++ = *it;
    }

    return out;
}

Move* generate_captures(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    Position work = pos;
    return generate_captures(work, mem, out);
}

Move* generate_quiets(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    GenInfo info{};
    init_gen_info(info, pos, mem);

    Move pseudo[MAX_MOVES];
    Move* mid = generate_pseudo_legal_with_info(pos, mem, info, pseudo);
    Position work = pos;

    for (Move* it = pseudo; it != mid; ++it) {
        if (move_is_capture(*it))
            continue;
        if (legal_fast_impl(work, mem, info, *it))
            *out++ = *it;
    }

    return out;
}

Move* generate_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    GenInfo info{};
    init_gen_info(info, pos, mem);

    if (info.in_check)
        return generate_evasions_with_info(pos, mem, info, out);

    return generate_non_evasions_with_info(pos, mem, info, out);
}

Move* generate_evasions(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    GenInfo info{};
    init_gen_info(info, pos, mem);

    if (!info.in_check)
        return generate_non_evasions_with_info(pos, mem, info, out);

    return generate_evasions_with_info(pos, mem, info, out);
}

Move* generate_pseudo_legal(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    return generate_pseudo_legal_with_info(pos, mem, info, out);
}

Move* generate_pseudo_legal(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    GenInfo info{};
    init_gen_info(info, pos, mem);
    return generate_pseudo_legal_with_info(pos, mem, info, out);
}

Move* generate_pseudo_captures(
    const Position& pos,
    const memory::Memory& mem,
    const GenInfo& info,
    Move* out
) noexcept {
    return info.in_check
        ? generate_evasions_with_info(pos, mem, info, out)
        : generate_capture_non_evasions_with_info(pos, mem, info, out);
}

Move* generate_pseudo_captures(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    GenInfo info{};
    init_gen_info(info, pos, mem);
    return generate_pseudo_captures(pos, mem, info, out);
}

Move* generate_legal(
    Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    // Generate pseudo-legal moves first, then keep only the moves that leave
    // the king safe according to the fast legality checker.
    GenInfo info{};
    init_gen_info(info, pos, mem);

    Move pseudo[MAX_MOVES];
    Move* mid = generate_pseudo_legal_with_info(pos, mem, info, pseudo);
    for (Move* it = pseudo; it != mid; ++it) {
        if (legal_fast_impl(pos, mem, info, *it))
            *out++ = *it;
    }

    return out;
}

Move* generate_legal(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept {
    Position work = pos;
    return generate_legal(work, mem, out);
}

Move* generate(
    const Position& pos,
    const memory::Memory& mem,
    Move* out,
    GenType type
) noexcept {
    switch (type) {
        case GEN_CAPTURES:     return generate_captures(pos, mem, out);
        case GEN_QUIETS:       return generate_quiets(pos, mem, out);
        case GEN_NON_EVASIONS: return generate_non_evasions(pos, mem, out);
        case GEN_EVASIONS:     return generate_evasions(pos, mem, out);
        case GEN_PSEUDO_LEGAL: return generate_pseudo_legal(pos, mem, out);
        case GEN_LEGAL:        return generate_legal(pos, mem, out);
        default:               return out;
    }
}

} // namespace valerain
