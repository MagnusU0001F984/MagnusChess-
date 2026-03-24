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

#include "Position.h"
#include "Evaluation.h"
#include "MoveGen.h"

#include <bit>

namespace valerain {

namespace {

inline void clear_castling_rights_by_square(Position& pos, Square sq) noexcept {
    switch (sq) {
        case 0:  pos.castling_rights &= ~WHITE_OOO; break;
        case 7:  pos.castling_rights &= ~WHITE_OO;  break;
        case 56: pos.castling_rights &= ~BLACK_OOO; break;
        case 63: pos.castling_rights &= ~BLACK_OO;  break;
        default: break;
    }
}

inline void remove_piece_at(Position& pos, Square sq) noexcept {
    const Piece pc = piece_on(pos, sq);
    if (pc == PIECE_NONE) return;

    position_remove_piece(pos, color_of(pc), type_of(pc), sq);
}

} // namespace

void position_clear(Position& pos) noexcept {
    pos.side_to_move = WHITE;
    pos.ep_sq = NO_SQ;
    pos.castling_rights = 0;
    pos.halfmove_clock = 0;
    pos.fullmove_number = 1;

    pos.king_sq[WHITE] = NO_SQ;
    pos.king_sq[BLACK] = NO_SQ;

    pos.color_bb[WHITE] = 0ULL;
    pos.color_bb[BLACK] = 0ULL;

    for (int pt = 0; pt < PIECE_NB; ++pt)
        pos.piece_bb[pt] = 0ULL;

    pos.occupied = 0ULL;
    pos.eval_mg[WHITE] = 0;
    pos.eval_mg[BLACK] = 0;
    pos.eval_eg[WHITE] = 0;
    pos.eval_eg[BLACK] = 0;
    pos.eval_phase = 0;

    for (int sq = 0; sq < SQ_NB; ++sq)
        pos.board[sq] = PIECE_NONE;
}

void position_recompute_occupied(Position& pos) noexcept {
    pos.occupied = pos.color_bb[WHITE] | pos.color_bb[BLACK];
}

void position_refresh_king_squares(Position& pos) noexcept {
    pos.king_sq[WHITE] = NO_SQ;
    pos.king_sq[BLACK] = NO_SQ;

    const Bitboard wk = pos.color_bb[WHITE] & pos.piece_bb[KING];
    const Bitboard bk = pos.color_bb[BLACK] & pos.piece_bb[KING];

    if (wk) pos.king_sq[WHITE] = static_cast<Square>(std::countr_zero(wk));
    if (bk) pos.king_sq[BLACK] = static_cast<Square>(std::countr_zero(bk));
}

void position_put_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    const Bitboard bb = bb_of(sq);

    pos.color_bb[color] |= bb;
    pos.piece_bb[piece_type] |= bb;
    pos.occupied |= bb;
    pos.board[sq] = static_cast<int>(make_piece(color, piece_type));
    eval::on_piece_added(pos, color, piece_type, sq);

    if (piece_type == KING)
        pos.king_sq[color] = sq;
}

void position_remove_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    const Bitboard bb = bb_of(sq);

    pos.color_bb[color] &= ~bb;
    pos.piece_bb[piece_type] &= ~bb;
    pos.occupied &= ~bb;
    pos.board[sq] = PIECE_NONE;
    eval::on_piece_removed(pos, color, piece_type, sq);

    if (piece_type == KING)
        pos.king_sq[color] = NO_SQ;
}

void position_move_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    const Bitboard from_bb = bb_of(from);
    const Bitboard to_bb   = bb_of(to);

    pos.color_bb[color] ^= from_bb | to_bb;
    pos.piece_bb[piece_type] ^= from_bb | to_bb;
    pos.occupied ^= from_bb | to_bb;

    pos.board[from] = PIECE_NONE;
    pos.board[to] = static_cast<int>(make_piece(color, piece_type));
    eval::on_piece_moved(pos, color, piece_type, from, to);

    if (piece_type == KING)
        pos.king_sq[color] = to;
}

bool position_has_valid_kings(const Position& pos) noexcept {
    const Bitboard wk = pos.color_bb[WHITE] & pos.piece_bb[KING];
    const Bitboard bk = pos.color_bb[BLACK] & pos.piece_bb[KING];
    return std::popcount(wk) == 1 && std::popcount(bk) == 1;
}

bool position_board_matches_bitboards(const Position& pos) noexcept {
    Bitboard color_occ[COLOR_NB]{};
    Bitboard piece_occ[PIECE_NB]{};

    for (int sq = 0; sq < SQ_NB; ++sq) {
        const Piece pc = static_cast<Piece>(pos.board[sq]);
        if (pc == PIECE_NONE) continue;

        const Color c = color_of(pc);
        const PieceType pt = type_of(pc);

        color_occ[c] |= bb_of(sq);
        piece_occ[pt] |= bb_of(sq);
    }

    return color_occ[WHITE] == pos.color_bb[WHITE] &&
           color_occ[BLACK] == pos.color_bb[BLACK] &&
           piece_occ[PAWN]   == pos.piece_bb[PAWN] &&
           piece_occ[KNIGHT] == pos.piece_bb[KNIGHT] &&
           piece_occ[BISHOP] == pos.piece_bb[BISHOP] &&
           piece_occ[ROOK]   == pos.piece_bb[ROOK] &&
           piece_occ[QUEEN]  == pos.piece_bb[QUEEN] &&
           piece_occ[KING]   == pos.piece_bb[KING] &&
           (pos.color_bb[WHITE] | pos.color_bb[BLACK]) == pos.occupied;
}

void do_move_copy(Position& pos, Move m) noexcept {
    const Color us = static_cast<Color>(pos.side_to_move);
    const Color them = (us == WHITE ? BLACK : WHITE);

    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const u16 flag    = move_flag(m);

    const Piece moving = piece_on(pos, from);
    const PieceType pt = type_of(moving);

    if (pt == PAWN || move_is_capture(m) || move_is_ep(m))
        pos.halfmove_clock = 0;
    else
        ++pos.halfmove_clock;

    if (us == BLACK)
        ++pos.fullmove_number;

    pos.ep_sq = NO_SQ;

    clear_castling_rights_by_square(pos, from);
    clear_castling_rights_by_square(pos, to);

    if (pt == KING) {
        if (us == WHITE) pos.castling_rights &= ~(WHITE_OO | WHITE_OOO);
        else             pos.castling_rights &= ~(BLACK_OO | BLACK_OOO);
    }

    if (flag == MOVE_OO) {
        if (us == WHITE) {
            position_move_piece(pos, WHITE, KING, 4, 6);
            position_move_piece(pos, WHITE, ROOK, 7, 5);
        } else {
            position_move_piece(pos, BLACK, KING, 60, 62);
            position_move_piece(pos, BLACK, ROOK, 63, 61);
        }
    }
    else if (flag == MOVE_OOO) {
        if (us == WHITE) {
            position_move_piece(pos, WHITE, KING, 4, 2);
            position_move_piece(pos, WHITE, ROOK, 0, 3);
        } else {
            position_move_piece(pos, BLACK, KING, 60, 58);
            position_move_piece(pos, BLACK, ROOK, 56, 59);
        }
    }
    else if (flag == MOVE_EP) {
        const Square cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
        remove_piece_at(pos, cap_sq);
        position_move_piece(pos, us, PAWN, from, to);
    }
    else if (move_is_promotion(m)) {
        if (move_is_capture(m))
            remove_piece_at(pos, to);

        position_remove_piece(pos, us, PAWN, from);
        position_put_piece(pos, us, promo_piece(m), to);
    }
    else {
        if (move_is_capture(m))
            remove_piece_at(pos, to);

        position_move_piece(pos, us, pt, from, to);

        if (flag == MOVE_DOUBLE_PUSH)
            pos.ep_sq = (us == WHITE) ? (from + 8) : (from - 8);
    }

    pos.side_to_move = them;
}

} // namespace valerain
