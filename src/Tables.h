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

#include <array>
#include <cstdint>

#include "Types.h"

namespace valerain {

struct ZobristTables {
    Key piece[COLOR_NB][PIECE_NB][SQ_NB]{};
    Key castling[16]{};
    Key ep_file[8]{};
    Key side = 0;
};

struct Tables {
    Bitboard king_attacks[SQ_NB]{};
    Bitboard knight_attacks[SQ_NB]{};
    Bitboard pawn_attacks[COLOR_NB][SQ_NB]{};

    Bitboard between[SQ_NB][SQ_NB]{};
    Bitboard line[SQ_NB][SQ_NB]{};

    u8 chebyshev[SQ_NB][SQ_NB]{};
    u8 manhattan[SQ_NB][SQ_NB]{};

    ZobristTables zobrist{};
    bool initialized = false;
};

constexpr int abs_i(int x) noexcept {
    return x < 0 ? -x : x;
}

constexpr int sign_i(int x) noexcept {
    return (x > 0) - (x < 0);
}

constexpr int chebyshev_distance(Square a, Square b) noexcept {
    const int df = abs_i(file_of(a) - file_of(b));
    const int dr = abs_i(rank_of(a) - rank_of(b));
    return df > dr ? df : dr;
}

constexpr int manhattan_distance(Square a, Square b) noexcept {
    const int df = abs_i(file_of(a) - file_of(b));
    const int dr = abs_i(rank_of(a) - rank_of(b));
    return df + dr;
}

inline u64 splitmix64(u64& x) noexcept {
    u64 z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline u64 mix64(u64 x) noexcept {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

constexpr Bitboard king_attack_mask(Square sq) noexcept {
    Bitboard b = 0;
    const int f = file_of(sq);
    const int r = rank_of(sq);

    for (int df = -1; df <= 1; ++df) {
        for (int dr = -1; dr <= 1; ++dr) {
            if (df == 0 && dr == 0) continue;
            const int nf = f + df;
            const int nr = r + dr;
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                b |= bb_of(nr * 8 + nf);
        }
    }

    return b;
}

constexpr Bitboard knight_attack_mask(Square sq) noexcept {
    Bitboard b = 0;
    const int f = file_of(sq);
    const int r = rank_of(sq);

    constexpr int D[8][2] = {
        { 1,  2}, { 2,  1}, { 2, -1}, { 1, -2},
        {-1, -2}, {-2, -1}, {-2,  1}, {-1,  2}
    };

    for (int i = 0; i < 8; ++i) {
        const int nf = f + D[i][0];
        const int nr = r + D[i][1];
        if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
            b |= bb_of(nr * 8 + nf);
    }

    return b;
}

constexpr Bitboard pawn_attack_mask(Color c, Square sq) noexcept {
    Bitboard b = 0;
    const int f = file_of(sq);
    const int r = rank_of(sq);

    if (c == WHITE) {
        if (f > 0 && r < 7) b |= bb_of((r + 1) * 8 + (f - 1));
        if (f < 7 && r < 7) b |= bb_of((r + 1) * 8 + (f + 1));
    } else {
        if (f > 0 && r > 0) b |= bb_of((r - 1) * 8 + (f - 1));
        if (f < 7 && r > 0) b |= bb_of((r - 1) * 8 + (f + 1));
    }

    return b;
}

constexpr bool aligned_line(Square a, Square b) noexcept {
    const int df = file_of(b) - file_of(a);
    const int dr = rank_of(b) - rank_of(a);
    return df == 0 || dr == 0 || abs_i(df) == abs_i(dr);
}

constexpr Bitboard compute_line(Square a, Square b) noexcept {
    if (a == b) return bb_of(a);
    if (!aligned_line(a, b)) return 0ULL;

    const int df = sign_i(file_of(b) - file_of(a));
    const int dr = sign_i(rank_of(b) - rank_of(a));
    const int step = dr * 8 + df;

    Bitboard mask = bb_of(a);
    int sq = a;

    while (sq != b) {
        sq += step;
        mask |= bb_of(sq);
    }

    return mask;
}

constexpr Bitboard compute_between(Square a, Square b) noexcept {
    if (a == b) return 0ULL;
    if (!aligned_line(a, b)) return 0ULL;

    const int df = sign_i(file_of(b) - file_of(a));
    const int dr = sign_i(rank_of(b) - rank_of(a));
    const int step = dr * 8 + df;

    Bitboard mask = 0ULL;
    int sq = a + step;

    while (sq != b) {
        mask |= bb_of(sq);
        sq += step;
    }

    return mask;
}

inline void tables_init_leapers(Tables& t) noexcept {
    for (int sq = 0; sq < SQ_NB; ++sq) {
        t.king_attacks[sq] = king_attack_mask(sq);
        t.knight_attacks[sq] = knight_attack_mask(sq);
        t.pawn_attacks[WHITE][sq] = pawn_attack_mask(WHITE, sq);
        t.pawn_attacks[BLACK][sq] = pawn_attack_mask(BLACK, sq);
    }
}

inline void tables_init_geometry(Tables& t) noexcept {
    for (int a = 0; a < SQ_NB; ++a) {
        for (int b = 0; b < SQ_NB; ++b) {
            t.between[a][b] = compute_between(a, b);
            t.line[a][b] = compute_line(a, b);
            t.chebyshev[a][b] = static_cast<u8>(chebyshev_distance(a, b));
            t.manhattan[a][b] = static_cast<u8>(manhattan_distance(a, b));
        }
    }
}

inline void tables_init_zobrist(ZobristTables& z, u64 seed = 0xC0FFEE1234567890ULL) noexcept {
    u64 x = seed;

    for (int c = 0; c < COLOR_NB; ++c)
        for (int pt = 0; pt < PIECE_NB; ++pt)
            for (int sq = 0; sq < SQ_NB; ++sq)
                z.piece[c][pt][sq] = splitmix64(x);

    for (int i = 0; i < 16; ++i)
        z.castling[i] = splitmix64(x);

    for (int i = 0; i < 8; ++i)
        z.ep_file[i] = splitmix64(x);

    z.side = splitmix64(x);
}

inline void tables_init(Tables& t, u64 zobrist_seed = 0xC0FFEE1234567890ULL) noexcept {
    tables_init_leapers(t);
    tables_init_geometry(t);
    tables_init_zobrist(t.zobrist, zobrist_seed);
    t.initialized = true;
}

} // namespace valerain