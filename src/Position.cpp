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
#include "Nnue.h"
#include "Tables.h"

#include <bit>

/*
This file owns board-state mutation. It keeps mailbox, bitboards, king caches,
incremental evaluation, and optional incremental Zobrist hashing consistent
through the same small set of mutator helpers.
*/

/* ===== 繁體中文註釋 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 實作詳情請參閱對應的 .h 標頭檔案。
 */

namespace magnus {

namespace {

constexpr int kMaterialNibbleBits = 4;
constexpr int kMaterialPiecesPerColor = 5;
constexpr u8 kNonKingMaterialWeight[PIECE_NB] = {
    1, 3, 3, 5, 9, 0
};

[[nodiscard]] inline bool tracks_material(PieceType piece_type) noexcept {
    return piece_type >= PAWN && piece_type <= QUEEN;
}

[[nodiscard]] inline int material_shift(Color color, PieceType piece_type) noexcept {
    return static_cast<int>(color) * (kMaterialPiecesPerColor * kMaterialNibbleBits)
        + static_cast<int>(piece_type) * kMaterialNibbleBits;
}

inline void refresh_material_signature(
    Position& pos,
    Color color,
    PieceType piece_type
) noexcept {
    if (!tracks_material(piece_type))
        return;

    const int shift = material_shift(color, piece_type);
    const Key mask = static_cast<Key>(0xFULL) << shift;
    pos.material_signature =
        (pos.material_signature & ~mask)
        | (static_cast<Key>(pos.piece_counts[color][piece_type]) << shift);
}

inline void add_piece_to_caches(
    Position& pos,
    Color color,
    PieceType piece_type
) noexcept {
    pos.piece_counts[color][piece_type] = static_cast<u8>(
        static_cast<int>(pos.piece_counts[color][piece_type]) + 1
    );

    if (!tracks_material(piece_type))
        return;

    pos.non_king_material = static_cast<u8>(
        non_king_material(pos) + static_cast<int>(kNonKingMaterialWeight[piece_type])
    );
    refresh_material_signature(pos, color, piece_type);
}

inline void remove_piece_from_caches(
    Position& pos,
    Color color,
    PieceType piece_type
) noexcept {
    pos.piece_counts[color][piece_type] = static_cast<u8>(
        static_cast<int>(pos.piece_counts[color][piece_type]) - 1
    );

    if (!tracks_material(piece_type))
        return;

    pos.non_king_material = static_cast<u8>(
        non_king_material(pos) - static_cast<int>(kNonKingMaterialWeight[piece_type])
    );
    refresh_material_signature(pos, color, piece_type);
}

/*
These helpers keep the incremental Zobrist key maintenance localized. The
plain board mutators stay oblivious to hashing, while the keyed copy-make path
can layer hash updates on top of the same structural operations.
*/
inline void key_xor_piece(
    Position& pos,
    const Tables& tables,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    pos.key ^= tables.zobrist.piece[color][piece_type][sq];
}

inline void key_xor_castling(Position& pos, const Tables& tables) noexcept {
    pos.key ^= tables.zobrist.castling[pos.castling_rights];
}

inline void key_xor_ep(Position& pos, const Tables& tables) noexcept {
    if (has_ep(pos))
        pos.key ^= tables.zobrist.ep_file[file_of(pos.ep_sq)];
}

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

inline void remove_piece_at(
    Position& pos,
    const Tables& tables,
    Square sq
) noexcept {
    const Piece pc = piece_on(pos, sq);
    if (pc == PIECE_NONE) return;

    const Color color = color_of(pc);
    const PieceType piece_type = type_of(pc);
    key_xor_piece(pos, tables, color, piece_type, sq);
    position_remove_piece(pos, color, piece_type, sq);
}

inline void move_piece_with_key(
    Position& pos,
    const Tables& tables,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    key_xor_piece(pos, tables, color, piece_type, from);
    key_xor_piece(pos, tables, color, piece_type, to);
    position_move_piece(pos, color, piece_type, from, to);
}

inline void put_piece_with_key(
    Position& pos,
    const Tables& tables,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    key_xor_piece(pos, tables, color, piece_type, sq);
    position_put_piece(pos, color, piece_type, sq);
}

} // namespace

/*
 * 棋盤狀態操作實作
 * 維護信箱 (mailbox)、位元棋盤、國王快取、增量評估、Zobrist 鍵值的一致性
 * make_move/unmake_move — 走棋/悔棋（含狀態恢復）
 * do_move_copy — 在新副本上走棋（用於根節點）
 * position_put_piece/position_remove_piece — 低層棋子操作
 * position_refresh_key — 重新計算 Zobrist 鍵值
 */
void position_clear(Position& pos) noexcept {
    // Reset every representation so the position can be rebuilt from scratch.
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
    for (auto& counts : pos.piece_counts)
        counts.fill(0);

    pos.occupied = 0ULL;
    pos.non_king_material = 0;
    pos.material_signature = 0ULL;
    pos.eval_mg[WHITE] = 0;
    pos.eval_mg[BLACK] = 0;
    pos.eval_eg[WHITE] = 0;
    pos.eval_eg[BLACK] = 0;
    pos.eval_phase = 0;
    pos.key = 0;
    pos.nnue_generation = 0;
    pos.nnue_acc_valid = false;
    for (auto& acc : pos.nnue_acc)
        acc.fill(0);

    for (int sq = 0; sq < SQ_NB; ++sq)
        pos.board[sq] = PIECE_NONE;

    nnue::on_position_cleared(pos);
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

Key position_compute_key(const Position& pos, const Tables& tables) noexcept {
    // Full recomputation is used for validation and root refreshes. Search
    // normally relies on the incremental key maintained by do_move_copy(..., tables).
    Key key = 0;

    for (int color = WHITE; color <= BLACK; ++color) {
        for (int piece_type = PAWN; piece_type <= KING; ++piece_type) {
            Bitboard bb = pieces(
                pos,
                static_cast<Color>(color),
                static_cast<PieceType>(piece_type)
            );

            while (bb) {
                const Square sq = static_cast<Square>(std::countr_zero(bb));
                key ^= tables.zobrist.piece[color][piece_type][sq];
                bb &= bb - 1;
            }
        }
    }

    key ^= tables.zobrist.castling[pos.castling_rights];

    if (has_ep(pos))
        key ^= tables.zobrist.ep_file[file_of(pos.ep_sq)];

    if (pos.side_to_move == BLACK)
        key ^= tables.zobrist.side;

    return key;
}

void position_refresh_key(Position& pos, const Tables& tables) noexcept {
    pos.key = position_compute_key(pos, tables);
}

void position_put_piece(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    // Update mailbox, bitboards, occupancy, king square cache, and eval cache
    // together so all board views remain consistent.
    const Bitboard bb = bb_of(sq);

    pos.color_bb[color] |= bb;
    pos.piece_bb[piece_type] |= bb;
    pos.occupied |= bb;
    add_piece_to_caches(pos, color, piece_type);
    pos.board[sq] = static_cast<int>(make_piece(color, piece_type));
    eval::on_piece_added(pos, color, piece_type, sq);
    nnue::on_piece_added(pos, color, piece_type, sq);

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
    remove_piece_from_caches(pos, color, piece_type);
    pos.board[sq] = PIECE_NONE;
    eval::on_piece_removed(pos, color, piece_type, sq);
    nnue::on_piece_removed(pos, color, piece_type, sq);

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
    // A move on the same piece type can be expressed as xor on the source and
    // destination bits, which keeps the update branch-free.
    const Bitboard from_bb = bb_of(from);
    const Bitboard to_bb   = bb_of(to);

    pos.color_bb[color] ^= from_bb | to_bb;
    pos.piece_bb[piece_type] ^= from_bb | to_bb;
    pos.occupied ^= from_bb | to_bb;

    pos.board[from] = PIECE_NONE;
    pos.board[to] = static_cast<int>(make_piece(color, piece_type));
    eval::on_piece_moved(pos, color, piece_type, from, to);
    nnue::on_piece_moved(pos, color, piece_type, from, to);

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
    std::array<std::array<u8, PIECE_NB>, COLOR_NB> piece_counts{};

    for (int sq = 0; sq < SQ_NB; ++sq) {
        const Piece pc = static_cast<Piece>(pos.board[sq]);
        if (pc == PIECE_NONE) continue;

        const Color c = color_of(pc);
        const PieceType pt = type_of(pc);

        color_occ[c] |= bb_of(sq);
        piece_occ[pt] |= bb_of(sq);
        piece_counts[c][pt] = static_cast<u8>(static_cast<int>(piece_counts[c][pt]) + 1);
    }

    int expected_non_king_material = 0;
    Key expected_material_signature = 0ULL;
    for (int color = WHITE; color <= BLACK; ++color) {
        const Color piece_color = static_cast<Color>(color);
        for (int piece_type = PAWN; piece_type <= KING; ++piece_type) {
            const PieceType pt = static_cast<PieceType>(piece_type);
            if (piece_counts[piece_color][pt] != pos.piece_counts[piece_color][pt])
                return false;

            if (!tracks_material(pt))
                continue;

            expected_non_king_material +=
                static_cast<int>(piece_counts[piece_color][pt])
                * static_cast<int>(kNonKingMaterialWeight[pt]);
            expected_material_signature |=
                static_cast<Key>(piece_counts[piece_color][pt])
                << material_shift(piece_color, pt);
        }
    }

    return color_occ[WHITE] == pos.color_bb[WHITE] &&
           color_occ[BLACK] == pos.color_bb[BLACK] &&
           piece_occ[PAWN]   == pos.piece_bb[PAWN] &&
           piece_occ[KNIGHT] == pos.piece_bb[KNIGHT] &&
           piece_occ[BISHOP] == pos.piece_bb[BISHOP] &&
           piece_occ[ROOK]   == pos.piece_bb[ROOK] &&
           piece_occ[QUEEN]  == pos.piece_bb[QUEEN] &&
           piece_occ[KING]   == pos.piece_bb[KING] &&
           (pos.color_bb[WHITE] | pos.color_bb[BLACK]) == pos.occupied &&
           expected_non_king_material == non_king_material(pos) &&
           expected_material_signature == packed_material_signature(pos);
}

void make_move(
    Position& pos,
    Move m,
    const Tables& tables,
    StateInfo& st
) noexcept {
    st.castling_rights = pos.castling_rights;
    st.ep_sq = pos.ep_sq;
    st.halfmove_clock = pos.halfmove_clock;
    st.fullmove_number = pos.fullmove_number;
    st.key = pos.key;
    st.captured = PIECE_NONE;
    st.captured_sq = NO_SQ;

    const Color us = static_cast<Color>(pos.side_to_move);
    const Color them = static_cast<Color>(us ^ 1);

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

    key_xor_castling(pos, tables);
    key_xor_ep(pos, tables);

    pos.ep_sq = NO_SQ;

    clear_castling_rights_by_square(pos, from);
    clear_castling_rights_by_square(pos, to);

    if (pt == KING) {
        if (us == WHITE) pos.castling_rights &= ~(WHITE_OO | WHITE_OOO);
        else             pos.castling_rights &= ~(BLACK_OO | BLACK_OOO);
    }

    if (flag == MOVE_OO) {
        if (us == WHITE) {
            move_piece_with_key(pos, tables, WHITE, KING, 4, 6);
            move_piece_with_key(pos, tables, WHITE, ROOK, 7, 5);
        } else {
            move_piece_with_key(pos, tables, BLACK, KING, 60, 62);
            move_piece_with_key(pos, tables, BLACK, ROOK, 63, 61);
        }
    }
    else if (flag == MOVE_OOO) {
        if (us == WHITE) {
            move_piece_with_key(pos, tables, WHITE, KING, 4, 2);
            move_piece_with_key(pos, tables, WHITE, ROOK, 0, 3);
        } else {
            move_piece_with_key(pos, tables, BLACK, KING, 60, 58);
            move_piece_with_key(pos, tables, BLACK, ROOK, 56, 59);
        }
    }
    else if (flag == MOVE_EP) {
        const Square cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
        st.captured_sq = cap_sq;
        st.captured = piece_on(pos, cap_sq);
        remove_piece_at(pos, tables, cap_sq);
        move_piece_with_key(pos, tables, us, PAWN, from, to);
    }
    else if (move_is_promotion(m)) {
        if (move_is_capture(m)) {
            st.captured_sq = to;
            st.captured = piece_on(pos, to);
            remove_piece_at(pos, tables, to);
        }

        key_xor_piece(pos, tables, us, PAWN, from);
        position_remove_piece(pos, us, PAWN, from);
        put_piece_with_key(pos, tables, us, promo_piece(m), to);
    }
    else {
        if (move_is_capture(m)) {
            st.captured_sq = to;
            st.captured = piece_on(pos, to);
            remove_piece_at(pos, tables, to);
        }

        move_piece_with_key(pos, tables, us, pt, from, to);

        if (flag == MOVE_DOUBLE_PUSH)
            pos.ep_sq = (us == WHITE) ? (from + 8) : (from - 8);
    }

    pos.side_to_move = them;

    key_xor_castling(pos, tables);
    key_xor_ep(pos, tables);
    pos.key ^= tables.zobrist.side;
}

void unmake_move(
    Position& pos,
    Move m,
    const Tables& tables,
    const StateInfo& st
) noexcept {
    const Color them = static_cast<Color>(pos.side_to_move);
    const Color us = static_cast<Color>(them ^ 1);

    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const u16 flag    = move_flag(m);

    if (flag == MOVE_OO) {
        if (us == WHITE) {
            position_move_piece(pos, WHITE, KING, 6, 4);
            position_move_piece(pos, WHITE, ROOK, 5, 7);
        } else {
            position_move_piece(pos, BLACK, KING, 62, 60);
            position_move_piece(pos, BLACK, ROOK, 61, 63);
        }
    }
    else if (flag == MOVE_OOO) {
        if (us == WHITE) {
            position_move_piece(pos, WHITE, KING, 2, 4);
            position_move_piece(pos, WHITE, ROOK, 3, 0);
        } else {
            position_move_piece(pos, BLACK, KING, 58, 60);
            position_move_piece(pos, BLACK, ROOK, 59, 56);
        }
    }
    else if (flag == MOVE_EP) {
        position_move_piece(pos, us, PAWN, to, from);

        if (st.captured != PIECE_NONE && st.captured_sq != NO_SQ) {
            position_put_piece(
                pos,
                color_of(st.captured),
                type_of(st.captured),
                st.captured_sq
            );
        }
    }
    else if (move_is_promotion(m)) {
        const PieceType promo = promo_piece(m);
        position_remove_piece(pos, us, promo, to);
        position_put_piece(pos, us, PAWN, from);

        if (st.captured != PIECE_NONE && st.captured_sq == to) {
            position_put_piece(
                pos,
                color_of(st.captured),
                type_of(st.captured),
                to
            );
        }
    }
    else {
        const PieceType pt = piece_type_on(pos, to);
        position_move_piece(pos, us, pt, to, from);

        if (st.captured != PIECE_NONE && st.captured_sq == to) {
            position_put_piece(
                pos,
                color_of(st.captured),
                type_of(st.captured),
                to
            );
        }
    }

    pos.side_to_move = us;
    pos.castling_rights = st.castling_rights;
    pos.ep_sq = st.ep_sq;
    pos.halfmove_clock = st.halfmove_clock;
    pos.fullmove_number = st.fullmove_number;
    pos.key = st.key;
}

void do_move_copy(Position& pos, Move m) noexcept {
    // Plain copy-make used by perft and validation paths that do not need the
    // incremental Zobrist key.
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

void do_move_copy(Position& pos, Move m, const Tables& tables) noexcept {
    // Kept for copy-make call sites that still require keyed updates.
    StateInfo st{};
    make_move(pos, m, tables, st);
}

} // namespace magnus
