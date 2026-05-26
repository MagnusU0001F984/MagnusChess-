/*
MIT License

Copyright (c) 2026 MagnusU0001F984

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

/* ===== ANNOTATED: 繁體中文註釋已添加 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 詳細說明請參閱對應的 .cpp 實作檔案。
 */


#pragma once

#include <array>

#include "NnueLayout.h"
#include "Types.h"

namespace magnus {

struct Tables;

/*
Position is the engine's canonical board state. It keeps mailbox access for
simple piece lookup, bitboards for fast move generation, incremental eval
totals, and an incremental Zobrist key for search and TT use.
*/
/*
 * Position — 引擎的標準棋盤狀態
 * 維護信箱 (mailbox) 用於簡單棋子查詢、位元棋盤用於快速著法生成、
 * 增量評估快取、增量 Zobrist 鍵值供搜尋和 TT 使用。
 * NNUE 累加器儲存在 Position 中（每個視角一個），支援增量更新。
 * StateInfo 記錄一步棋的狀態變化，供 unmake_move 恢復。
 */
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
    std::array<std::array<u8, PIECE_NB>, COLOR_NB> piece_counts{};
    u8 non_king_material = 0;
    Key material_signature = 0ULL;

    int eval_mg[COLOR_NB]{};
    int eval_eg[COLOR_NB]{};
    int eval_phase = 0;
    Key key = 0;

    int board[SQ_NB];

    // NNUE keeps dual-perspective hidden-layer accumulators inside Position so
    // make/unmake and copy-make paths can share the same incremental state.
    mutable u32 nnue_generation = 0;
    mutable bool nnue_acc_valid = false;
    alignas(64) mutable std::array<std::array<i16, nnue::kHiddenSize>, COLOR_NB> nnue_acc{};

    // MNUE-P2 persistent accumulator. Separate from legacy Chess768 NNUE
    // because P2 uses hidden size 1024 and a different feature layout.
    mutable u32 mnue_p2_generation = 0;
    mutable u8 mnue_p2_acc_valid_mask = 0;
    alignas(64) mutable std::array<std::array<i16, 1024>, COLOR_NB> mnue_p2_acc{};
};

struct StateInfo {
    int castling_rights = 0;
    Square ep_sq = NO_SQ;
    int halfmove_clock = 0;
    int fullmove_number = 1;
    Key key = 0ULL;
    Piece captured = PIECE_NONE;
    Square captured_sq = NO_SQ;
};

// Convenience accessors used throughout the engine.
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

inline int piece_count(const Position& pos, Color color, PieceType pt) noexcept {
    return static_cast<int>(pos.piece_counts[color][pt]);
}

inline int non_king_material(const Position& pos) noexcept {
    return static_cast<int>(pos.non_king_material);
}

inline Key packed_material_signature(const Position& pos) noexcept {
    return pos.material_signature;
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
[[nodiscard]] Key position_compute_key(const Position& pos, const Tables& tables) noexcept;
void position_refresh_key(Position& pos, const Tables& tables) noexcept;

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

void make_move(
    Position& pos,
    Move m,
    const Tables& tables,
    StateInfo& st
) noexcept;

void unmake_move(
    Position& pos,
    Move m,
    const Tables& tables,
    const StateInfo& st
) noexcept;

// Copy-make helpers used by perft and legality validation paths. The overload
// with Tables keeps the incremental Zobrist key up to date as well.
void do_move_copy(Position& pos, Move m) noexcept;
void do_move_copy(Position& pos, Move m, const Tables& tables) noexcept;

} // namespace magnus
