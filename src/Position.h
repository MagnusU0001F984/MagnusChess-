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

namespace valerain {

struct Position {
    int side_to_move = WHITE;
    Square ep_sq = NO_SQ;
    int castling_rights = 0;
    int halfmove_clock = 0;
    int fullmove_number = 1;

    Square king_sq[COLOR_NB]{ NO_SQ, NO_SQ };

    Bitboard color_bb[COLOR_NB]{};
    Bitboard piece_bb[PIECE_NB]{};
    Bitboard occupied = 0ULL;

    int board[SQ_NB];
};

inline int us(const Position& pos) noexcept {
    return pos.side_to_move;
}

inline int them(const Position& pos) noexcept {
    return pos.side_to_move ^ 1;
}

inline Bitboard pieces(const Position& pos) noexcept {
    return pos.occupied;
}

inline Bitboard pieces(const Position& pos, Color color) noexcept {
    return pos.color_bb[color];
}

inline Bitboard pieces_of_type(const Position& pos, PieceType pt) noexcept {
    return pos.piece_bb[pt];
}

inline Bitboard pieces(const Position& pos, Color color, PieceType pt) noexcept {
    return pos.color_bb[color] & pos.piece_bb[pt];
}

inline Square king_square(const Position& pos, Color color) noexcept {
    return pos.king_sq[color];
}

inline bool has_ep(const Position& pos) noexcept {
    return pos.ep_sq != NO_SQ;
}

inline Piece piece_on(const Position& pos, Square sq) noexcept {
    return static_cast<Piece>(pos.board[sq]);
}

inline Color color_on(const Position& pos, Square sq) noexcept {
    const Piece pc = static_cast<Piece>(pos.board[sq]);
    return pc == PIECE_NONE ? COLOR_NONE : color_of(pc);
}

inline PieceType piece_type_on(const Position& pos, Square sq) noexcept {
    const Piece pc = static_cast<Piece>(pos.board[sq]);
    return pc == PIECE_NONE ? PIECE_TYPE_NONE : type_of(pc);
}

inline bool empty_on(const Position& pos, Square sq) noexcept {
    return pos.board[sq] == PIECE_NONE;
}

inline bool occupied_on(const Position& pos, Square sq) noexcept {
    return pos.board[sq] != PIECE_NONE;
}

void position_clear(Position& pos) noexcept;
void position_recompute_occupied(Position& pos) noexcept;
void position_refresh_king_squares(Position& pos) noexcept;

void position_put_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept;

void position_remove_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept;

void position_move_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept;

bool position_has_valid_kings(const Position& pos) noexcept;
bool position_board_matches_bitboards(const Position& pos) noexcept;
void do_move_copy(Position& pos, Move m) noexcept;

} // namespace valerain