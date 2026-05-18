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

#include "Attack.h"

#include <array>
#include <bit>
#include <vector>

#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)) \
 && (defined(__GNUC__) || defined(__clang__))
    #include <immintrin.h>
    #define MAGNUS_CAN_COMPILE_PEXT 1
    #define MAGNUS_TARGET_BMI2 __attribute__((target("bmi2")))
#else
    #define MAGNUS_CAN_COMPILE_PEXT 0
    #define MAGNUS_TARGET_BMI2
#endif

/*
This file implements the slider attack backends. It can fall back to classical
ray scans, use dense lookup tables, or exploit BMI2/PEXT when available.
*/

/* ===== 繁體中文註釋 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 實作詳情請參閱對應的 .h 標頭檔案。
 */

namespace magnus {

namespace {

using SliderAttackFn =
    AttackBitboard (*)(const memory::Memory&, int, AttackBitboard) noexcept;

struct AttackBackendVTable {
    SliderAttackFn bishop;
    SliderAttackFn rook;
    AttackBackendKind kind;
    const char* name;
    bool uses_slider_tables;
};

std::array<SliderAttackEntry, 64> g_bishop_entries{};
std::array<SliderAttackEntry, 64> g_rook_entries{};
std::vector<AttackBitboard> g_bishop_table;
std::vector<AttackBitboard> g_rook_table;
bool g_slider_tables_ready = false;


// March a ray square by square until the board edge or the first blocker.
inline AttackBitboard scan_ray(
    int sq,
    int df,
    int dr,
    AttackBitboard occupied
) noexcept {
    AttackBitboard attacks = 0ULL;

    int f = attack_file_of(sq) + df;
    int r = attack_rank_of(sq) + dr;

    while (f >= 0 && f < 8 && r >= 0 && r < 8) {
        const int to = r * 8 + f;
        const AttackBitboard to_bb = attack_bb_of(to);

        attacks |= to_bb;
        if (occupied & to_bb)
            break;

        f += df;
        r += dr;
    }

    return attacks;
}

inline AttackBitboard bishop_attacks_classical_impl(
    int sq,
    AttackBitboard occupied
) noexcept {
    return scan_ray(sq,  1,  1, occupied)
         | scan_ray(sq,  1, -1, occupied)
         | scan_ray(sq, -1,  1, occupied)
         | scan_ray(sq, -1, -1, occupied);
}

inline AttackBitboard rook_attacks_classical_impl(
    int sq,
    AttackBitboard occupied
) noexcept {
    return scan_ray(sq,  1,  0, occupied)
         | scan_ray(sq, -1,  0, occupied)
         | scan_ray(sq,  0,  1, occupied)
         | scan_ray(sq,  0, -1, occupied);
}

AttackBitboard bishop_attacks_classical(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    (void)mem;
    return bishop_attacks_classical_impl(sq, occupied);
}

AttackBitboard rook_attacks_classical(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    (void)mem;
    return rook_attacks_classical_impl(sq, occupied);
}

inline AttackBitboard bishop_relevant_mask(int sq) noexcept {
    AttackBitboard mask = 0ULL;
    const int file = attack_file_of(sq);
    const int rank = attack_rank_of(sq);

    for (int f = file + 1, r = rank + 1; f <= 6 && r <= 6; ++f, ++r)
        mask |= attack_bb_of(r * 8 + f);
    for (int f = file + 1, r = rank - 1; f <= 6 && r >= 1; ++f, --r)
        mask |= attack_bb_of(r * 8 + f);
    for (int f = file - 1, r = rank + 1; f >= 1 && r <= 6; --f, ++r)
        mask |= attack_bb_of(r * 8 + f);
    for (int f = file - 1, r = rank - 1; f >= 1 && r >= 1; --f, --r)
        mask |= attack_bb_of(r * 8 + f);

    return mask;
}

inline AttackBitboard rook_relevant_mask(int sq) noexcept {
    AttackBitboard mask = 0ULL;
    const int file = attack_file_of(sq);
    const int rank = attack_rank_of(sq);

    for (int f = file + 1; f <= 6; ++f)
        mask |= attack_bb_of(rank * 8 + f);
    for (int f = file - 1; f >= 1; --f)
        mask |= attack_bb_of(rank * 8 + f);
    for (int r = rank + 1; r <= 6; ++r)
        mask |= attack_bb_of(r * 8 + file);
    for (int r = rank - 1; r >= 1; --r)
        mask |= attack_bb_of(r * 8 + file);

    return mask;
}

inline AttackBitboard set_occupancy_from_index(
    std::uint32_t index,
    AttackBitboard mask
) noexcept {
    AttackBitboard occ = 0ULL;

    while (mask) {
        const AttackBitboard bit = mask & (~mask + 1ULL);
        mask ^= bit;
        if (index & 1U)
            occ |= bit;
        index >>= 1;
    }

    return occ;
}

inline std::uint32_t dense_index_from_occupied(
    AttackBitboard occupied,
    AttackBitboard mask
) noexcept {
    occupied &= mask;

    std::uint32_t index = 0;
    std::uint32_t bit_index = 0;

    while (mask) {
        const AttackBitboard bit = mask & (~mask + 1ULL);
        mask ^= bit;
        if (occupied & bit)
            index |= (1U << bit_index);
        ++bit_index;
    }

    return index;
}

#if MAGNUS_CAN_COMPILE_PEXT
MAGNUS_TARGET_BMI2 bool query_cpu_bmi2_support() noexcept {
    __builtin_cpu_init();
    return __builtin_cpu_supports("bmi2");
}

MAGNUS_TARGET_BMI2 std::uint32_t pext_index_from_occupied(
    AttackBitboard occupied,
    AttackBitboard mask
) noexcept {
    return static_cast<std::uint32_t>(_pext_u64(occupied, mask));
}

MAGNUS_TARGET_BMI2 AttackBitboard bishop_attacks_pext(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    (void)mem;
    const SliderAttackEntry& e = g_bishop_entries[sq];
    return g_bishop_table[e.offset + pext_index_from_occupied(occupied, e.mask)];
}

MAGNUS_TARGET_BMI2 AttackBitboard rook_attacks_pext(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    (void)mem;
    const SliderAttackEntry& e = g_rook_entries[sq];
    return g_rook_table[e.offset + pext_index_from_occupied(occupied, e.mask)];
}
#else
bool query_cpu_bmi2_support() noexcept {
    return false;
}
#endif

/*
 * 攻擊生成實作
 * init_slider_tables() — 構建稠密索引滑子攻擊表（主教+城堡）
 * set_occupancy_from_index() — 從索引還原佔位位元棋盤
 * dense_index_from_occupied() — 從佔位計算稠密索引（PEXT 或乘法雜湊）
 * attack_auto_select_backend() — 自動選擇最快可用的攻擊後端
 */
void init_slider_table(
    std::array<SliderAttackEntry, 64>& entries,
    std::vector<AttackBitboard>& table,
    AttackBitboard (*mask_fn)(int) noexcept,
    AttackBitboard (*attack_fn)(int, AttackBitboard) noexcept
) {
    // Build a dense occupancy-indexed lookup table for each source square.
    entries.fill({});

    std::size_t total_size = 0;
    for (int sq = 0; sq < 64; ++sq) {
        const AttackBitboard mask = mask_fn(sq);
        const auto relevant_bits = static_cast<std::uint8_t>(std::popcount(mask));
        entries[sq].mask = mask;
        entries[sq].offset = static_cast<std::uint32_t>(total_size);
        entries[sq].relevant_bits = relevant_bits;
        entries[sq].shift = static_cast<std::uint8_t>(64 - relevant_bits);
        total_size += (std::size_t{1} << relevant_bits);
    }

    table.assign(total_size, 0ULL);

    for (int sq = 0; sq < 64; ++sq) {
        const SliderAttackEntry& e = entries[sq];
        const std::uint32_t count = (1U << e.relevant_bits);

        for (std::uint32_t idx = 0; idx < count; ++idx) {
            const AttackBitboard occ = set_occupancy_from_index(idx, e.mask);
            table[e.offset + idx] = attack_fn(sq, occ);
        }
    }
}

void init_slider_tables() {
    if (g_slider_tables_ready)
        return;

    init_slider_table(g_bishop_entries, g_bishop_table,
                      &bishop_relevant_mask,
                      &bishop_attacks_classical_impl);
    init_slider_table(g_rook_entries, g_rook_table,
                      &rook_relevant_mask,
                      &rook_attacks_classical_impl);

    g_slider_tables_ready = true;
}

AttackBitboard bishop_attacks_table(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    (void)mem;
    const SliderAttackEntry& e = g_bishop_entries[sq];
    return g_bishop_table[e.offset + dense_index_from_occupied(occupied, e.mask)];
}

AttackBitboard rook_attacks_table(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    (void)mem;
    const SliderAttackEntry& e = g_rook_entries[sq];
    return g_rook_table[e.offset + dense_index_from_occupied(occupied, e.mask)];
}

bool pext_supported_runtime() noexcept {
    static const bool supported = query_cpu_bmi2_support();
    return supported;
}

AttackBackendVTable g_attack_backend {
    &bishop_attacks_classical,
    &rook_attacks_classical,
    AttackBackendKind::CLASSICAL,
    "classical",
    false
};

} // namespace

void attack_init_backend(memory::Memory& mem) noexcept {
    (void)mem;
    // Initialize shared tables once, then pick the fastest available backend.
    init_slider_tables();
    attack_auto_select_backend();
}

void attack_set_backend(AttackBackendKind kind) noexcept {
    switch (kind) {
        case AttackBackendKind::PEXT:
            init_slider_tables();
#if MAGNUS_CAN_COMPILE_PEXT
            if (pext_supported_runtime()) {
                g_attack_backend.bishop = &bishop_attacks_pext;
                g_attack_backend.rook   = &rook_attacks_pext;
                g_attack_backend.kind   = AttackBackendKind::PEXT;
                g_attack_backend.name   = "pext";
                g_attack_backend.uses_slider_tables = true;
                break;
            }
#endif
            [[fallthrough]];

        case AttackBackendKind::TABLE:
            init_slider_tables();
            g_attack_backend.bishop = &bishop_attacks_table;
            g_attack_backend.rook   = &rook_attacks_table;
            g_attack_backend.kind   = AttackBackendKind::TABLE;
            g_attack_backend.name   = "table";
            g_attack_backend.uses_slider_tables = true;
            break;

        case AttackBackendKind::MAGIC:
        case AttackBackendKind::CLASSICAL:
        default:
            g_attack_backend.bishop = &bishop_attacks_classical;
            g_attack_backend.rook   = &rook_attacks_classical;
            g_attack_backend.kind   = AttackBackendKind::CLASSICAL;
            g_attack_backend.name   = "classical";
            g_attack_backend.uses_slider_tables = false;
            break;
    }
}

void attack_auto_select_backend() noexcept {
    attack_set_backend(AttackBackendKind::PEXT);
}

AttackBackendKind attack_backend_kind() noexcept {
    return g_attack_backend.kind;
}

const char* attack_backend_name() noexcept {
    return g_attack_backend.name;
}

bool attack_backend_uses_slider_tables() noexcept {
    return g_attack_backend.uses_slider_tables;
}

bool attack_backend_pext_supported() noexcept {
    return pext_supported_runtime();
}

const SliderAttackEntry& bishop_slider_entry(int sq) noexcept {
    return g_bishop_entries[sq];
}

const SliderAttackEntry& rook_slider_entry(int sq) noexcept {
    return g_rook_entries[sq];
}

std::size_t bishop_slider_table_size() noexcept {
    return g_bishop_table.size();
}

std::size_t rook_slider_table_size() noexcept {
    return g_rook_table.size();
}

AttackBitboard bishop_attacks(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    return g_attack_backend.bishop(mem, sq, occupied);
}

AttackBitboard rook_attacks(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied
) noexcept {
    return g_attack_backend.rook(mem, sq, occupied);
}

AttackBitboard bishop_rays(int sq) noexcept {
    return scan_ray(sq,  1,  1, 0ULL)
         | scan_ray(sq,  1, -1, 0ULL)
         | scan_ray(sq, -1,  1, 0ULL)
         | scan_ray(sq, -1, -1, 0ULL);
}

AttackBitboard rook_rays(int sq) noexcept {
    return scan_ray(sq,  1,  0, 0ULL)
         | scan_ray(sq, -1,  0, 0ULL)
         | scan_ray(sq,  0,  1, 0ULL)
         | scan_ray(sq,  0, -1, 0ULL);
}

AttackBitboard bishop_xray_attacks(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied,
    AttackBitboard blockers
) noexcept {
    // X-ray attacks keep looking beyond the first screened blocker.
    const AttackBitboard attacks = bishop_attacks(mem, sq, occupied);
    const AttackBitboard screened = attacks & blockers;

    if (!screened)
        return attacks;

    return attacks ^ bishop_attacks(mem, sq, occupied ^ screened);
}

AttackBitboard rook_xray_attacks(
    const memory::Memory& mem,
    int sq,
    AttackBitboard occupied,
    AttackBitboard blockers
) noexcept {
    const AttackBitboard attacks = rook_attacks(mem, sq, occupied);
    const AttackBitboard screened = attacks & blockers;

    if (!screened)
        return attacks;

    return attacks ^ rook_attacks(mem, sq, occupied ^ screened);
}

bool same_file(int a, int b) noexcept {
    return attack_file_of(a) == attack_file_of(b);
}

bool same_rank(int a, int b) noexcept {
    return attack_rank_of(a) == attack_rank_of(b);
}

bool same_diagonal(int a, int b) noexcept {
    return abs_i(attack_file_of(a) - attack_file_of(b))
        == abs_i(attack_rank_of(a) - attack_rank_of(b));
}

bool aligned(int a, int b) noexcept {
    return same_file(a, b) || same_rank(a, b) || same_diagonal(a, b);
}

} // namespace magnus
