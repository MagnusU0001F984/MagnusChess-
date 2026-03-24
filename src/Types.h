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

#include <cstdint>

namespace valerain {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using Bitboard = u64;
using Key      = u64;
using Move     = u16;
using Square   = int;

constexpr int SQ_NB    = 64;
constexpr int FILE_NB  = 8;
constexpr int RANK_NB  = 8;
constexpr int COLOR_NB = 2;
constexpr int PIECE_NB = 6;

enum Color : int {
    WHITE = 0,
    BLACK = 1,
    COLOR_NONE = 2
};

enum PieceType : int {
    PAWN = 0,
    KNIGHT = 1,
    BISHOP = 2,
    ROOK = 3,
    QUEEN = 4,
    KING = 5,
    PIECE_TYPE_NB = 6,
    PIECE_TYPE_NONE = 7
};

enum Piece : int {
    W_PAWN   = 0,
    W_KNIGHT = 1,
    W_BISHOP = 2,
    W_ROOK   = 3,
    W_QUEEN  = 4,
    W_KING   = 5,

    B_PAWN   = 6,
    B_KNIGHT = 7,
    B_BISHOP = 8,
    B_ROOK   = 9,
    B_QUEEN  = 10,
    B_KING   = 11,

    PIECE_NONE = 12
};

enum CastlingRight : int {
    NO_CASTLING = 0,
    WHITE_OO    = 1 << 0,
    WHITE_OOO   = 1 << 1,
    BLACK_OO    = 1 << 2,
    BLACK_OOO   = 1 << 3,

    WHITE_CASTLING = WHITE_OO | WHITE_OOO,
    BLACK_CASTLING = BLACK_OO | BLACK_OOO,
    ANY_CASTLING   = WHITE_CASTLING | BLACK_CASTLING
};

constexpr Square NO_SQ = -1;

constexpr Bitboard bb_of(Square sq) noexcept {
    return 1ULL << sq;
}

constexpr int file_of(Square sq) noexcept {
    return sq & 7;
}

constexpr int rank_of(Square sq) noexcept {
    return sq >> 3;
}

constexpr bool on_board(Square sq) noexcept {
    return sq >= 0 && sq < 64;
}

constexpr Color operator~(Color c) noexcept {
    return c == WHITE ? BLACK : WHITE;
}

constexpr Piece make_piece(Color c, PieceType pt) noexcept {
    return c == WHITE ? static_cast<Piece>(pt)
                      : static_cast<Piece>(pt + 6);
}

constexpr Color color_of(Piece pc) noexcept {
    return pc == PIECE_NONE ? COLOR_NONE
                            : (pc < 6 ? WHITE : BLACK);
}

constexpr PieceType type_of(Piece pc) noexcept {
    return pc == PIECE_NONE ? PIECE_TYPE_NONE
                            : static_cast<PieceType>(static_cast<int>(pc) % 6);
}

constexpr bool is_ok(Color c) noexcept {
    return c == WHITE || c == BLACK;
}

constexpr bool is_ok(PieceType pt) noexcept {
    return pt >= PAWN && pt <= KING;
}

constexpr bool is_ok(Piece pc) noexcept {
    return pc >= W_PAWN && pc <= B_KING;
}

constexpr bool is_ok(Square sq) noexcept {
    return on_board(sq);
}

} // namespace valerain