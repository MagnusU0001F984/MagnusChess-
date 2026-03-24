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

//Memory 

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Types.h"
#include "Tables.h"
#include "TT.h"

namespace valerain::memory {

using ::valerain::u8;
using ::valerain::u16;
using ::valerain::u32;
using ::valerain::u64;
using ::valerain::i16;
using ::valerain::i32;
using ::valerain::i64;

using ::valerain::Bitboard;
using ::valerain::Key;
using ::valerain::Move;
using ::valerain::Square;
using ::valerain::Color;
using ::valerain::PieceType;
using ::valerain::Tables;
using ::valerain::mix64;

using ::valerain::SQ_NB;
using ::valerain::FILE_NB;
using ::valerain::RANK_NB;
using ::valerain::COLOR_NB;
using ::valerain::PIECE_NB;

template <typename T>
inline std::size_t pow2_capacity_from_bytes(std::size_t bytes) noexcept {
    std::size_t n = bytes / sizeof(T);
    if (n == 0) n = 1;
    return std::bit_ceil(n);
}

struct PawnHashEntry {
    Key key = 0;
    i16 mg_score = 0;
    i16 eg_score = 0;
    u16 passed_files = 0;
    u16 weak_files = 0;
    Bitboard passed_pawns[COLOR_NB]{};
    Bitboard pawn_attack_span[COLOR_NB]{};
};

struct PawnTable {
    PawnHashEntry* entries = nullptr;
    std::size_t count = 0;
    std::size_t mask = 0;
};

inline void pawn_table_free(PawnTable& t) noexcept {
    delete[] t.entries;
    t.entries = nullptr;
    t.count = 0;
    t.mask = 0;
}

inline void pawn_table_clear(PawnTable& t) noexcept {
    for (std::size_t i = 0; i < t.count; ++i)
        t.entries[i] = PawnHashEntry{};
}

inline void pawn_table_resize_mb(PawnTable& t, std::size_t mb) {
    pawn_table_free(t);
    const std::size_t bytes = mb * 1024ULL * 1024ULL;
    const std::size_t count = pow2_capacity_from_bytes<PawnHashEntry>(bytes);
    t.entries = new PawnHashEntry[count]{};
    t.count = count;
    t.mask = count - 1;
}

struct MaterialHashEntry {
    Key key = 0;
    i16 phase = 0;
    i16 imbalance = 0;
    i16 scale = 0;
    u16 flags = 0;
};

struct MaterialTable {
    MaterialHashEntry* entries = nullptr;
    std::size_t count = 0;
    std::size_t mask = 0;
};

inline void material_table_free(MaterialTable& t) noexcept {
    delete[] t.entries;
    t.entries = nullptr;
    t.count = 0;
    t.mask = 0;
}

inline void material_table_clear(MaterialTable& t) noexcept {
    for (std::size_t i = 0; i < t.count; ++i)
        t.entries[i] = MaterialHashEntry{};
}

inline void material_table_resize_mb(MaterialTable& t, std::size_t mb) {
    material_table_free(t);
    const std::size_t bytes = mb * 1024ULL * 1024ULL;
    const std::size_t count = pow2_capacity_from_bytes<MaterialHashEntry>(bytes);
    t.entries = new MaterialHashEntry[count]{};
    t.count = count;
    t.mask = count - 1;
}

struct Memory {
    Tables tables{};
    TT tt{};
    PawnTable pawn{};
    MaterialTable material{};
};

inline void memory_init(
    Memory& mem,
    std::size_t tt_mb = 64,
    std::size_t pawn_mb = 8,
    std::size_t material_mb = 2,
    u64 zobrist_seed = 0xC0FFEE1234567890ULL
) {
    if (!mem.tables.initialized)
        ::valerain::tables_init(mem.tables, zobrist_seed);

    tt_resize_mb(mem.tt, tt_mb);
    pawn_table_resize_mb(mem.pawn, pawn_mb);
    material_table_resize_mb(mem.material, material_mb);
}

inline void memory_clear_hash(Memory& mem) noexcept {
    tt_clear(mem.tt);
    pawn_table_clear(mem.pawn);
    material_table_clear(mem.material);
}

inline void memory_new_search(Memory& mem) noexcept {
    tt_new_search(mem.tt);
}

inline void memory_free(Memory& mem) noexcept {
    tt_free(mem.tt);
    pawn_table_free(mem.pawn);
    material_table_free(mem.material);
}

} // namespace valerain::memory