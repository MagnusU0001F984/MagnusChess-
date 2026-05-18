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

/*
Middlegame and endgame scores are stored separately so the evaluator can keep
base material and PSQT terms distinct until the final blend. Phase is tracked
on a 24-point scale and the score is interpolated as
(mg * phase + eg * (24 - phase)) / 24. The PSQT tables are white-oriented, so
black pieces reuse the same values by flipping squares vertically with sq ^ 56.
*/

#include "Evaluation.h"

#include <array>

/* ===== 繁體中文註釋 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 實作詳情請參閱對應的 .h 標頭檔案。
 */

namespace magnus::eval {

namespace {

// Each PST entry stores separate middlegame and endgame terms.
struct StageScore {
    int mg;
    int eg;
};

#define S(mg, eg) StageScore{ (mg), (eg) }

constexpr StageScore PieceValue[PIECE_TYPE_NB] = {
    S(  82, 144),  // PAWN
    S( 337, 281),  // KNIGHT
    S( 365, 297),  // BISHOP
    S( 477, 512),  // ROOK
    S(1025, 936),  // QUEEN
    S(   0,   0)   // KING
};

constexpr int mg_value[PIECE_TYPE_NB] = {
    PieceValue[PAWN].mg,
    PieceValue[KNIGHT].mg,
    PieceValue[BISHOP].mg,
    PieceValue[ROOK].mg,
    PieceValue[QUEEN].mg,
    PieceValue[KING].mg
};

constexpr int eg_value[PIECE_TYPE_NB] = {
    PieceValue[PAWN].eg,
    PieceValue[KNIGHT].eg,
    PieceValue[BISHOP].eg,
    PieceValue[ROOK].eg,
    PieceValue[QUEEN].eg,
    PieceValue[KING].eg
};

constexpr int phase_inc[PIECE_TYPE_NB] = {
    0, // PAWN
    1, // KNIGHT
    1, // BISHOP
    2, // ROOK
    4, // QUEEN
    0  // KING
};

constexpr int TOTAL_PHASE = 24;

// 兵的 PST 微调：加强中心控制，减少开局对挺兵的恐惧
constexpr StageScore PawnPST[SQ_NB] = {
    S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0),
    S( -5,  13), S( 10,   8), S( 10,   8), S(-10,  10), S(-10,  13), S( 10,   0), S( 10,   2), S( -5,  -7),
    S( -5,   4), S(  5,   7), S( 10,  -6), S( 15,   1), S( 15,   0), S( 10,  -5), S(  5,  -1), S( -5,  -8),
    S(  0,  13), S( 10,   9), S( 25,  -3), S( 45,  -7), S( 45,  -7), S( 25,  -8), S( 10,   3), S(  0,  -1), // 中心挺起加分
    S(  5,  32), S( 15,  24), S( 30,  13), S( 40,   5), S( 40,  -2), S( 30,   4), S( 15,  17), S(  5,  17),
    S( -6,  94), S(  7, 100), S( 26,  85), S( 31,  67), S( 65,  56), S( 56,  53), S( 25,  82), S(-20,  84),
    S( 98, 178), S(134, 173), S( 61, 158), S( 95, 134), S( 68, 147), S(126, 132), S( 34, 165), S(-11, 187),
    S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0)
};

// 马的 PST 微调：降低中心格过度奖励，鼓励更合理的开发
constexpr StageScore KnightPST[SQ_NB] = {
    S(-160, -70), S(-100, -40), S( -70, -20), S( -50, -10), S( -50, -10), S( -70, -20), S(-100, -40), S(-160, -70),
    S( -70, -35), S( -20, -10), S(   0,   0), S(  10,   8), S(  10,   8), S(   0,   0), S( -20, -10), S( -70, -35),
    S( -30, -15), S(  10,   4), S(  20,  12), S(  30,  18), S(  30,  18), S(  20,  12), S(  10,   4), S( -30, -15),
    S( -15,  -8), S(  15,   8), S(  30,  18), S(  38,  24), S(  38,  24), S(  30,  18), S(  15,   8), S( -15,  -8), // 60 -> 38 降温
    S( -15,  -8), S(  15,   8), S(  30,  18), S(  38,  24), S(  38,  24), S(  30,  18), S(  15,   8), S( -15,  -8),
    S( -30, -15), S(  10,   4), S(  25,  12), S(  30,  18), S(  30,  18), S(  25,  12), S(  10,   4), S( -30, -15),
    S( -70, -35), S( -20, -10), S(   0,   0), S(  10,   8), S(  10,   8), S(   0,   0), S( -20, -10), S( -70, -35),
    S(-160, -70), S(-100, -40), S( -70, -20), S( -50, -10), S( -50, -10), S( -70, -20), S(-100, -40), S(-160, -70)
};

constexpr StageScore BishopPST[SQ_NB] = {
    S(-33, -23), S( -3,  -9), S(-14, -23), S(-21,  -5), S(-13,  -9), S(-12, -16), S(-39,  -5), S(-21, -17),
    S(  4, -14), S( 15, -18), S( 16,  -7), S(  0,  -1), S(  7,   4), S( 21,  -9), S( 33, -15), S(  1, -27),
    S(  0, -12), S( 15,  -3), S( 15,   8), S( 15,  10), S( 14,  13), S( 27,   3), S( 18,  -7), S( 10, -15),
    S( -6,  -6), S( 13,   3), S( 13,  13), S( 26,  19), S( 34,   7), S( 12,  10), S( 10,  -3), S(  4,  -9),
    S( -4,  -3), S(  5,   9), S( 19,  12), S( 50,   9), S( 37,  14), S( 37,  10), S(  7,   3), S( -2,   2),
    S(-16,   2), S( 37,  -8), S( 43,   0), S( 40,  -1), S( 35,  -2), S( 50,   6), S( 37,   0), S( -2,   4),
    S(-26,  -8), S( 16,  -4), S(-18,   7), S(-13, -12), S( 30,  -3), S( 59, -13), S( 18,  -4), S(-47, -14),
    S(-29, -14), S(  4, -21), S(-82, -11), S(-37,  -8), S(-25,  -7), S(-42,  -9), S(  7, -17), S( -8, -24)
};

constexpr StageScore RookPST[SQ_NB] = {
    S(-19,  -9), S(-13,   2), S(  1,   3), S( 17,  -1), S( 16,  -5), S(  7, -13), S(-37,   4), S(-26, -20),
    S(-44,  -6), S(-16,  -6), S(-20,   0), S( -9,   2), S( -1,  -9), S( 11,  -9), S( -6, -11), S(-71,  -3),
    S(-45,  -4), S(-25,   0), S(-16,  -5), S(-17,  -1), S(  3,  -7), S(  0, -12), S( -5,  -8), S(-33, -16),
    S(-36,   3), S(-26,   5), S(-12,   8), S( -1,   4), S(  9,  -5), S( -7,  -6), S(  6,  -8), S(-23, -11),
    S(-24,   4), S(-11,   3), S(  7,  13), S( 26,   1), S( 24,   2), S( 35,   1), S( -8,  -1), S(-20,   2),
    S( -5,   7), S( 19,   7), S( 26,   7), S( 36,   5), S( 17,   4), S( 45,  -3), S( 61,  -5), S( 16,  -3),
    S( 27,  11), S( 32,  13), S( 58,  13), S( 62,  11), S( 80,  -3), S( 67,   3), S( 26,   8), S( 44,   3),
    S( 32,  13), S( 42,  10), S( 32,  18), S( 51,  15), S( 63,  12), S(  9,  12), S( 31,   8), S( 43,   5)
};

constexpr StageScore QueenPST[SQ_NB] = {
    S( -1, -33), S(-18, -28), S( -9, -22), S( 10, -43), S(-15,  -5), S(-25, -32), S(-31, -20), S(-50, -41),
    S(-35, -22), S( -8, -23), S( 11, -30), S(  2, -16), S(  8, -16), S( 15, -23), S( -3, -36), S(  1, -32),
    S(-14, -16), S(  2, -27), S(-11,  15), S( -2,   6), S( -5,   9), S(  2,  17), S( 14,  10), S(  5,   5),
    S( -9, -18), S(-26,  28), S( -9,  19), S(-10,  47), S( -2,  31), S( -4,  34), S(  3,  39), S( -3,  23),
    S(-27,   3), S(-27,  22), S(-16,  24), S(-16,  45), S( -1,  57), S( 17,  40), S( -2,  57), S(  1,  36),
    S(-13, -20), S(-17,   6), S(  7,   9), S(  8,  49), S( 29,  47), S( 56,  35), S( 47,  19), S( 57,   9),
    S(-24, -17), S(-39,  20), S( -5,  32), S(  1,  41), S(-16,  58), S( 57,  25), S( 28,  30), S( 54,   0),
    S(-28,  -9), S(  0,  22), S( 29,  22), S( 12,  27), S( 59,  27), S( 44,  19), S( 43,  10), S( 45,  20)
};

constexpr StageScore KingPST[SQ_NB] = {
    S(-28, -40), S(-36, -12), S(-36,   0), S(-44,  10), S(-44,  10), S(-36,   0), S(-36, -12), S(-28, -40),
    S(-28, -22), S(-36,   0), S(-36,  10), S(-44,  20), S(-44,  20), S(-36,  10), S(-36,   0), S(-28, -22),
    S(-28, -18), S(-36,  10), S(-36,  20), S(-44,  30), S(-44,  30), S(-36,  20), S(-36,  10), S(-28, -18),
    S(-28, -14), S(-36,  18), S(-36,  28), S(-44,  38), S(-44,  38), S(-36,  28), S(-36,  18), S(-28, -14),
    S(-18, -10), S(-26,  18), S(-26,  26), S(-34,  34), S(-34,  34), S(-26,  26), S(-26,  18), S(-18, -10),
    S( -8, -10), S(-16,  10), S(-16,  18), S(-20,  24), S(-20,  24), S(-16,  18), S(-16,  10), S( -8, -10),
    S( 20, -30), S( 18, -10), S(  0,   0), S(  0,   8), S(  0,   8), S(  0,   0), S( 18, -10), S( 20, -30),
    S( 24, -50), S( 30, -30), S( 12, -20), S(  0, -10), S(  0, -10), S( 12, -20), S( 30, -30), S( 24, -50)
};

constexpr const StageScore* PST[PIECE_TYPE_NB] = {
    PawnPST, KnightPST, BishopPST, RookPST, QueenPST, KingPST
};

#undef S

using PsqtTable = std::array<std::array<int, SQ_NB>, PIECE_TYPE_NB>;

// Material and PST are merged once into lookup tables so incremental updates
// only need a single table access per phase.
constexpr PsqtTable build_mg_table() noexcept {
    PsqtTable table{};

    for (int pt = 0; pt < PIECE_TYPE_NB; ++pt)
        for (int sq = 0; sq < SQ_NB; ++sq)
            table[pt][sq] = mg_value[pt] + PST[pt][sq].mg;

    return table;
}

constexpr PsqtTable build_eg_table() noexcept {
    PsqtTable table{};

    for (int pt = 0; pt < PIECE_TYPE_NB; ++pt)
        for (int sq = 0; sq < SQ_NB; ++sq)
            table[pt][sq] = eg_value[pt] + PST[pt][sq].eg;

    return table;
}

constexpr PsqtTable mg_table = build_mg_table();
constexpr PsqtTable eg_table = build_eg_table();

constexpr Square flip_vertical(Square sq) noexcept {
    return sq ^ 56;
}

constexpr Square orient_square(Color color, Square sq) noexcept {
    return color == WHITE ? sq : flip_vertical(sq);
}

// Incremental hooks update the cached tapered totals stored inside Position.
inline void add_piece_value(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    const int side = static_cast<int>(color);
    const Square table_sq = orient_square(color, sq);

    pos.eval_mg[side] += mg_table[piece_type][table_sq];
    pos.eval_eg[side] += eg_table[piece_type][table_sq];
    pos.eval_phase += phase_inc[piece_type];
}

inline void remove_piece_value(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    const int side = static_cast<int>(color);
    const Square table_sq = orient_square(color, sq);

    pos.eval_mg[side] -= mg_table[piece_type][table_sq];
    pos.eval_eg[side] -= eg_table[piece_type][table_sq];
    pos.eval_phase -= phase_inc[piece_type];
}

} // namespace

void on_piece_added(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    add_piece_value(pos, color, piece_type, sq);
}

void on_piece_removed(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    remove_piece_value(pos, color, piece_type, sq);
}

void on_piece_moved(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    const int side = static_cast<int>(color);
    const Square from_sq = orient_square(color, from);
    const Square to_sq = orient_square(color, to);

    pos.eval_mg[side] += mg_table[piece_type][to_sq] - mg_table[piece_type][from_sq];
    pos.eval_eg[side] += eg_table[piece_type][to_sq] - eg_table[piece_type][from_sq];
}

Score evaluate(const Position& pos) noexcept {
    // Blend the middlegame and endgame scores according to the current phase,
    // then report the result from the side-to-move point of view.
    int phase = pos.eval_phase;
    if (phase < 0)
        phase = 0;
    if (phase > TOTAL_PHASE)
        phase = TOTAL_PHASE;

    const int mg_score = pos.eval_mg[WHITE] - pos.eval_mg[BLACK];
    const int eg_score = pos.eval_eg[WHITE] - pos.eval_eg[BLACK];
    const int white_pov =
        (mg_score * phase + eg_score * (TOTAL_PHASE - phase)) / TOTAL_PHASE;

    return pos.side_to_move == WHITE ? white_pov : -white_pov;
}

} // namespace magnus::eval
