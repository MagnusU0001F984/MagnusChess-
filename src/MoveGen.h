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

#include "Types.h"
#include "Memory.h"
#include "Position.h"

namespace valerain {

/*
Move generation combines bitboards, attack tables, pin masks, and a final
legality filter. The code keeps a simple external API while reusing the same
helpers for captures, evasions, and full legal move lists.
*/

constexpr int MAX_MOVES = 256;

enum GenType : int {
    GEN_CAPTURES = 0,
    GEN_QUIETS,
    GEN_NON_EVASIONS,
    GEN_EVASIONS,
    GEN_PSEUDO_LEGAL,
    GEN_LEGAL
};

/*
    16-bit move layout

    bits  0..5   : to
    bits  6..11  : from
    bits 12..15  : flag
*/
enum MoveFlag : u16 {
    MOVE_QUIET        = 0,
    MOVE_DOUBLE_PUSH  = 1,
    MOVE_OO           = 2,
    MOVE_OOO          = 3,
    MOVE_CAPTURE      = 4,
    MOVE_EP           = 5,

    MOVE_PROMO_N      = 8,
    MOVE_PROMO_B      = 9,
    MOVE_PROMO_R      = 10,
    MOVE_PROMO_Q      = 11,

    MOVE_CAP_PROMO_N  = 12,
    MOVE_CAP_PROMO_B  = 13,
    MOVE_CAP_PROMO_R  = 14,
    MOVE_CAP_PROMO_Q  = 15
};

struct MoveList {
    // Plain move container used by perft and search.
    Move moves[MAX_MOVES];
    int size = 0;
};

struct ScoredMove {
    Move move = 0;
    i32 score = 0;
};

struct ScoredMoveList {
    ScoredMove moves[MAX_MOVES];
    int size = 0;
};

struct GenInfo {
    // Precomputed side-to-move context shared by the different generators.
    Color us = WHITE;
    Color them = BLACK;

    Square king_sq = NO_SQ;
    Square ep_sq = NO_SQ;

    Bitboard occupied = 0ULL;
    Bitboard us_occ = 0ULL;
    Bitboard them_occ = 0ULL;
    Bitboard empty = 0ULL;

    Bitboard checkers = 0ULL;
    Bitboard pinned = 0ULL;
    Bitboard pinners = 0ULL;
    Bitboard danger = 0ULL;

    Bitboard capture_mask = 0ULL;
    Bitboard push_mask = 0ULL;

    bool in_check = false;
    bool double_check = false;
};

// Move decoding helpers for the compact 16-bit move format.
constexpr Square from_sq(Move m) noexcept {
    return static_cast<Square>((m >> 6) & 63);
}

constexpr Square to_sq(Move m) noexcept {
    return static_cast<Square>(m & 63);
}

constexpr u16 move_flag(Move m) noexcept {
    return static_cast<u16>((m >> 12) & 15);
}

constexpr Move make_move(Square from, Square to, u16 flag = MOVE_QUIET) noexcept {
    return static_cast<Move>((to & 63) | ((from & 63) << 6) | ((flag & 15) << 12));
}

constexpr bool move_is_none(Move m) noexcept {
    return m == 0;
}

constexpr bool move_is_capture(Move m) noexcept {
    const u16 f = move_flag(m);
    return f == MOVE_CAPTURE || f == MOVE_EP || f >= MOVE_CAP_PROMO_N;
}

constexpr bool move_is_promotion(Move m) noexcept {
    return move_flag(m) >= MOVE_PROMO_N;
}

constexpr bool move_is_underpromotion(Move m) noexcept {
    const u16 f = move_flag(m);
    return f == MOVE_PROMO_N || f == MOVE_PROMO_B || f == MOVE_PROMO_R ||
           f == MOVE_CAP_PROMO_N || f == MOVE_CAP_PROMO_B || f == MOVE_CAP_PROMO_R;
}

constexpr bool move_is_castle(Move m) noexcept {
    const u16 f = move_flag(m);
    return f == MOVE_OO || f == MOVE_OOO;
}

constexpr bool move_is_ep(Move m) noexcept {
    return move_flag(m) == MOVE_EP;
}

constexpr bool move_is_double_push(Move m) noexcept {
    return move_flag(m) == MOVE_DOUBLE_PUSH;
}

constexpr PieceType promo_piece(Move m) noexcept {
    switch (move_flag(m)) {
        case MOVE_PROMO_N:
        case MOVE_CAP_PROMO_N: return KNIGHT;
        case MOVE_PROMO_B:
        case MOVE_CAP_PROMO_B: return BISHOP;
        case MOVE_PROMO_R:
        case MOVE_CAP_PROMO_R: return ROOK;
        case MOVE_PROMO_Q:
        case MOVE_CAP_PROMO_Q: return QUEEN;
        default: return PIECE_TYPE_NONE;
    }
}

inline void movelist_clear(MoveList& list) noexcept {
    list.size = 0;
}

inline void scored_movelist_clear(ScoredMoveList& list) noexcept {
    list.size = 0;
}

inline void movelist_push(MoveList& list, Move m) noexcept {
    list.moves[list.size++] = m;
}

inline void scored_movelist_push(ScoredMoveList& list, Move m, i32 score) noexcept {
    list.moves[list.size++] = {m, score};
}

/*
    Attack and king-safety helpers
*/
Bitboard attackers_to(
    const Position& pos,
    const memory::Memory& mem,
    Square sq,
    Bitboard occupied
) noexcept;

Bitboard attackers_to_color(
    const Position& pos,
    const memory::Memory& mem,
    Square sq,
    Color by,
    Bitboard occupied
) noexcept;

Bitboard pinned_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color us
) noexcept;

Bitboard pinners_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color us
) noexcept;

Bitboard checkers_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color us
) noexcept;

Bitboard danger_bb(
    const Position& pos,
    const memory::Memory& mem,
    Color by
) noexcept;

void init_gen_info(
    GenInfo& info,
    const Position& pos,
    const memory::Memory& mem
) noexcept;

/*
    Move legality
*/
bool pseudo_legal(
    const Position& pos,
    const memory::Memory& mem,
    Move m
) noexcept;

bool legal(
    const Position& pos,
    const memory::Memory& mem,
    Move m
) noexcept;

/*
    Core generation API
*/
Move* generate_captures(
    Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_captures(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_quiets(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_evasions(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_pseudo_legal(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_legal(
    Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

Move* generate_legal(
    const Position& pos,
    const memory::Memory& mem,
    Move* out
) noexcept;

inline void generate_captures(
    Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_captures(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_captures(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_captures(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_quiets(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_quiets(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_non_evasions(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_non_evasions(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_evasions(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_evasions(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_pseudo_legal(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_pseudo_legal(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_legal(
    Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_legal(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

inline void generate_legal(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list
) noexcept {
    Move* end = generate_legal(pos, mem, list.moves);
    list.size = static_cast<int>(end - list.moves);
}

Move* generate(
    const Position& pos,
    const memory::Memory& mem,
    Move* out,
    GenType type
) noexcept;

inline void generate(
    const Position& pos,
    const memory::Memory& mem,
    MoveList& list,
    GenType type
) noexcept {
    Move* end = generate(pos, mem, list.moves, type);
    list.size = static_cast<int>(end - list.moves);
}

} // namespace valerain
