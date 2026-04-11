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

#include "Nnue.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <immintrin.h>
#include <string>

namespace valerain::nnue {

namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u32 = std::uint32_t;

constexpr u32 kMagic   = 0x554E4E56u; // "VNNU" in little-endian file bytes
constexpr u32 kVersion = 1;

constexpr int kInputs = kInputSize;
constexpr int kHidden = kHiddenSize;
constexpr int kClip   = kActivationClip;
constexpr int kDefaultScale = 400;

struct FileHeader {
    u32 magic;
    u32 version;
    u32 input_size;
    u32 hidden_size;
    i32 scale;
};

struct NativeNetwork {
    bool is_loaded = false;
    bool is_bullet_simple = false;
    std::string loaded_path;
    std::string desc;
    std::string error;

    i32 scale = kDefaultScale;

    std::array<i16, kInputs * kHidden> w0{};
    std::array<i16, kHidden> b0{};

    std::array<i16, 2 * kHidden> w1{};
    i16 b1 = 0;
};

NativeNetwork g_net{};
u32 g_generation = 1;

constexpr int kWinRateMaterialMin = 17;
constexpr int kWinRateMaterialMax = 78;
constexpr int kCpLookupMaxRaw = 32768;
constexpr int kMaterialBucketCount = kWinRateMaterialMax - kWinRateMaterialMin + 1;
constexpr double kWinRateAs[] = {
    -72.32565836, 185.93832038, -144.58862193, 416.44950446
};
constexpr double kWinRateBs[] = {
    83.86794042, -136.06112997, 69.98820887, 47.62901433
};
using CpLookupRow = std::array<i16, kCpLookupMaxRaw + 1>;
using CpLookupTable = std::array<CpLookupRow, kMaterialBucketCount>;

[[nodiscard]] CpLookupTable build_cp_lookup_table() {
    CpLookupTable lut{};
    for (int material = kWinRateMaterialMin; material <= kWinRateMaterialMax; ++material) {
        const double m = static_cast<double>(material) / 58.0;
        const double a =
            (((kWinRateAs[0] * m + kWinRateAs[1]) * m + kWinRateAs[2]) * m)
            + kWinRateAs[3];
        auto& row = lut[static_cast<std::size_t>(material - kWinRateMaterialMin)];
        for (int raw = 0; raw <= kCpLookupMaxRaw; ++raw) {
            row[static_cast<std::size_t>(raw)] = static_cast<i16>(std::llround(
                (100.0 * static_cast<double>(raw)) / a
            ));
        }
    }
    return lut;
}

const CpLookupTable g_cp_lookup = build_cp_lookup_table();

template<typename T>
[[nodiscard]] T read_le(std::istream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return value;
}

[[nodiscard]] inline Color opposite(Color c) noexcept {
    return c == WHITE ? BLACK : WHITE;
}

[[nodiscard]] inline Square flip_vertical_sq(Square sq) noexcept {
    return static_cast<Square>(static_cast<int>(sq) ^ 56);
}

[[nodiscard]] inline Square orient_square(Square sq, Color persp) noexcept {
    return persp == WHITE ? sq : flip_vertical_sq(sq);
}

// Maps PAWN..KING -> 0..5.
// Adjust this switch if your engine uses different enum values.
[[nodiscard]] inline int piece_plane(PieceType pt) noexcept {
    switch (pt) {
        case PAWN:   return 0;
        case KNIGHT: return 1;
        case BISHOP: return 2;
        case ROOK:   return 3;
        case QUEEN:  return 4;
        case KING:   return 5;
        default:     return -1;
    }
}

[[nodiscard]] inline int chess768_index(Color persp, Piece pc, Square sq) noexcept {
    const PieceType pt = type_of(pc);
    const int plane = piece_plane(pt);
    if (plane < 0)
        return -1;

    const Color pc_color = color_of(pc);
    const int color_plane = (pc_color == persp) ? 0 : 1;
    const Square osq = orient_square(sq, persp);

    return (color_plane * 6 + plane) * 64 + static_cast<int>(osq);
}

[[nodiscard]] inline bool accumulator_matches(const Position& pos) noexcept {
    return pos.nnue_acc_valid && pos.nnue_generation == g_generation;
}

[[nodiscard]] inline int win_rate_material(const Position& pos) noexcept {
    const int material =
        std::popcount(pieces(pos, WHITE, PAWN)   | pieces(pos, BLACK, PAWN)) +
    3 * std::popcount(pieces(pos, WHITE, KNIGHT) | pieces(pos, BLACK, KNIGHT)) +
    3 * std::popcount(pieces(pos, WHITE, BISHOP) | pieces(pos, BLACK, BISHOP)) +
    5 * std::popcount(pieces(pos, WHITE, ROOK)   | pieces(pos, BLACK, ROOK)) +
    9 * std::popcount(pieces(pos, WHITE, QUEEN)  | pieces(pos, BLACK, QUEEN));
    return std::clamp(material, kWinRateMaterialMin, kWinRateMaterialMax);
}

[[nodiscard]] inline int lookup_cp(int raw, int material) noexcept {
    const auto& row =
        g_cp_lookup[static_cast<std::size_t>(material - kWinRateMaterialMin)];
    const i64 abs_raw = raw >= 0 ? static_cast<i64>(raw) : -static_cast<i64>(raw);
    const int index = static_cast<int>(std::min<i64>(abs_raw, kCpLookupMaxRaw));
    const int cp = static_cast<int>(row[static_cast<std::size_t>(index)]);
    return raw >= 0 ? cp : -cp;
}

inline void invalidate_accumulator(Position& pos) noexcept {
    pos.nnue_acc_valid = false;
    pos.nnue_generation = g_generation;
}

#if defined(__AVX2__)
inline void apply_feature_delta_avx2(
    std::array<i16, kHidden>& acc,
    const i16* weights,
    bool add
) noexcept {
    i16* acc_ptr = acc.data();
    for (int i = 0; i < kHidden; i += 16) {
        __m256i acc_vec =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc_ptr + i));
        const __m256i weight_vec =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));

        if (add) {
            acc_vec = _mm256_add_epi16(acc_vec, weight_vec);
        } else {
            acc_vec = _mm256_sub_epi16(acc_vec, weight_vec);
        }

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc_ptr + i), acc_vec);
    }
}

inline void apply_feature_move_delta_avx2(
    std::array<i16, kHidden>& acc,
    const i16* add_weights,
    const i16* sub_weights
) noexcept {
    i16* acc_ptr = acc.data();
    for (int i = 0; i < kHidden; i += 16) {
        __m256i acc_vec =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc_ptr + i));
        const __m256i add_vec =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(add_weights + i));
        const __m256i sub_vec =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sub_weights + i));

        acc_vec = _mm256_add_epi16(acc_vec, _mm256_sub_epi16(add_vec, sub_vec));

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc_ptr + i), acc_vec);
    }
}

#endif

inline void apply_feature_delta(
    std::array<i16, kHidden>& acc,
    int feature_index,
    int delta
) noexcept {
    const i16* w = &g_net.w0[static_cast<std::size_t>(feature_index) * kHidden];
#if defined(__AVX2__)
    apply_feature_delta_avx2(acc, w, delta > 0);
    return;
#endif
    if (delta > 0) {
        for (int i = 0; i < kHidden; ++i)
            acc[i] = static_cast<i16>(static_cast<i32>(acc[i]) + static_cast<i32>(w[i]));
        return;
    }

    for (int i = 0; i < kHidden; ++i)
        acc[i] = static_cast<i16>(static_cast<i32>(acc[i]) - static_cast<i32>(w[i]));
}

inline void apply_feature_move_delta(
    std::array<i16, kHidden>& acc,
    int add_feature_index,
    int sub_feature_index
) noexcept {
    const i16* add = &g_net.w0[static_cast<std::size_t>(add_feature_index) * kHidden];
    const i16* sub = &g_net.w0[static_cast<std::size_t>(sub_feature_index) * kHidden];
#if defined(__AVX2__)
    apply_feature_move_delta_avx2(acc, add, sub);
    return;
#endif
    for (int i = 0; i < kHidden; ++i)
        acc[i] = static_cast<i16>(
            static_cast<i32>(acc[i]) +
            static_cast<i32>(add[i]) -
            static_cast<i32>(sub[i])
        );
}

inline void apply_piece_delta(
    Position& pos,
    Color piece_color,
    PieceType piece_type,
    Square sq,
    int delta
) noexcept {
    const Piece pc = make_piece(piece_color, piece_type);
    for (int persp = WHITE; persp <= BLACK; ++persp) {
        const int idx = chess768_index(static_cast<Color>(persp), pc, sq);
        if (idx >= 0)
            apply_feature_delta(pos.nnue_acc[persp], idx, delta);
    }
}

inline void apply_piece_move_delta(
    Position& pos,
    Color piece_color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    const Piece pc = make_piece(piece_color, piece_type);
    for (int persp = WHITE; persp <= BLACK; ++persp) {
        const Color perspective = static_cast<Color>(persp);
        const int sub_idx = chess768_index(perspective, pc, from);
        const int add_idx = chess768_index(perspective, pc, to);
        if (sub_idx >= 0 && add_idx >= 0) {
            apply_feature_move_delta(pos.nnue_acc[persp], add_idx, sub_idx);
            continue;
        }

        if (sub_idx >= 0)
            apply_feature_delta(pos.nnue_acc[persp], sub_idx, -1);
        if (add_idx >= 0)
            apply_feature_delta(pos.nnue_acc[persp], add_idx, 1);
    }
}

void rebuild_accumulator(const Position& pos) noexcept {
    for (int persp = WHITE; persp <= BLACK; ++persp)
        pos.nnue_acc[persp] = g_net.b0;

    Bitboard bb = pieces(pos);
    while (bb) {
        const Square sq =
            static_cast<Square>(std::countr_zero(static_cast<std::uint64_t>(bb)));
        bb &= (bb - 1);

        const Piece pc = piece_on(pos, sq);
        if (pc == PIECE_NONE)
            continue;

        for (int persp = WHITE; persp <= BLACK; ++persp) {
            const int idx = chess768_index(static_cast<Color>(persp), pc, sq);
            if (idx < 0)
                continue;

            apply_feature_delta(pos.nnue_acc[persp], idx, 1);
        }
    }

    pos.nnue_generation = g_generation;
    pos.nnue_acc_valid = true;
}

inline void ensure_accumulator(const Position& pos) noexcept {
    if (!accumulator_matches(pos))
        rebuild_accumulator(pos);
}

void clear_network() noexcept {
    ++g_generation;
    if (g_generation == 0)
        ++g_generation;

    g_net.is_loaded = false;
    g_net.loaded_path.clear();
    g_net.desc.clear();
    g_net.error.clear();
    g_net.scale = kDefaultScale;
    g_net.is_bullet_simple = false;
    g_net.w0.fill(0);
    g_net.b0.fill(0);
    g_net.w1.fill(0);
    g_net.b1 = 0;
}

[[nodiscard]] inline i32 screlu(i32 x) noexcept {
    const i32 y = std::clamp(x, 0, kClip);
    return y * y;
}

[[nodiscard]] int forward(const Position& pos) noexcept {
    const Color stm = static_cast<Color>(pos.side_to_move);
    const Color nstm = opposite(stm);
    ensure_accumulator(pos);
    const auto& us = pos.nnue_acc[stm];
    const auto& them = pos.nnue_acc[nstm];
    i32 sum = 0;

    for (int i = 0; i < kHidden; ++i) {
        sum += static_cast<i32>(g_net.w1[i]) * screlu(static_cast<i32>(us[i]));
        sum += static_cast<i32>(g_net.w1[kHidden + i]) * screlu(static_cast<i32>(them[i]));
    }

    sum /= kClip;                     // divide by QA
    sum += static_cast<i32>(g_net.b1);
    sum *= g_net.scale;              // multiply by SCALE
    sum /= (kClip * 64);             // divide by QA * QB

    return static_cast<int>(sum);
}

bool load_bullet_simple_quantised(const std::string& path) {
    unload();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        g_net.error = "cannot open NNUE file: " + path;
        return false;
    }

    // Rust layout for simple.rs quantised.bin:
    // feature_weights: [Accumulator; 768]  where Accumulator = [i16; 128], align 64
    // feature_bias:    [i16; 128]
    // output_weights:  [i16; 256]
    // output_bias:     i16
    // trailing padding to struct alignment (64 bytes)

    in.read(reinterpret_cast<char*>(g_net.w0.data()), sizeof(g_net.w0));
    in.read(reinterpret_cast<char*>(g_net.b0.data()), sizeof(g_net.b0));
    in.read(reinterpret_cast<char*>(g_net.w1.data()), sizeof(g_net.w1));
    in.read(reinterpret_cast<char*>(&g_net.b1), sizeof(g_net.b1));

    if (!in) {
        clear_network();
        g_net.error = "truncated Bullet quantised.bin";
        return false;
    }

    // ignore any trailing alignment padding
    g_net.scale = kDefaultScale;
    g_net.is_loaded = true;
    g_net.is_bullet_simple = true;
    g_net.loaded_path = path;
    g_net.desc = "Bullet simple quantised NNUE (Chess768 dual-perspective 128x1)";
    return true;
}

} // namespace

bool load(const std::string& path) {
    unload();

    if (path.size() >= 4 && path.substr(path.size() - 4) == ".bin") {
        return load_bullet_simple_quantised(path);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        g_net.error = "cannot open NNUE file: " + path;
        return false;
    }

    const FileHeader h{
        .magic = read_le<u32>(in),
        .version = read_le<u32>(in),
        .input_size = read_le<u32>(in),
        .hidden_size = read_le<u32>(in),
        .scale = read_le<i32>(in)
    };

    if (!in) {
        g_net.error = "failed to read NNUE header";
        return false;
    }

    if (h.magic != kMagic) {
        g_net.error = "bad NNUE magic";
        return false;
    }

    if (h.version != kVersion) {
        g_net.error = "unsupported NNUE version";
        return false;
    }

    if (h.input_size != kInputs || h.hidden_size != kHidden) {
        g_net.error = "network dimensions mismatch";
        return false;
    }

    g_net.scale = h.scale > 0 ? h.scale : kDefaultScale;

    in.read(reinterpret_cast<char*>(g_net.w0.data()), sizeof(g_net.w0));
    in.read(reinterpret_cast<char*>(g_net.b0.data()), sizeof(g_net.b0));
    in.read(reinterpret_cast<char*>(g_net.w1.data()), sizeof(g_net.w1));
    in.read(reinterpret_cast<char*>(&g_net.b1), sizeof(g_net.b1));

    if (!in) {
        clear_network();
        g_net.error = "truncated NNUE file";
        return false;
    }

    g_net.is_loaded = true;
    g_net.is_bullet_simple = false;
    g_net.loaded_path = path;
    g_net.desc = "Valerain native NNUE v1 (Chess768 dual-perspective 128x1)";
    return true;
}

void unload() noexcept {
    clear_network();
}

bool loaded() noexcept {
    return g_net.is_loaded;
}

const std::string& path() noexcept {
    return g_net.loaded_path;
}

const std::string& description() noexcept {
    return g_net.desc;
}

const std::string& last_error() noexcept {
    return g_net.error;
}

int eval(const Position& pos) noexcept {
    if (!g_net.is_loaded)
        return 0;
    return forward(pos);
}

void on_position_cleared(Position& pos) noexcept {
    invalidate_accumulator(pos);
}

void on_piece_added(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    if (!g_net.is_loaded || !accumulator_matches(pos))
        return;

    apply_piece_delta(pos, color, piece_type, sq, 1);
}

void on_piece_removed(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    if (!g_net.is_loaded || !accumulator_matches(pos))
        return;

    apply_piece_delta(pos, color, piece_type, sq, -1);
}

void on_piece_moved(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    if (!g_net.is_loaded || !accumulator_matches(pos))
        return;

    apply_piece_move_delta(pos, color, piece_type, from, to);
}

WinRateParams win_rate_params(const Position& pos) noexcept {
    const double m = static_cast<double>(win_rate_material(pos)) / 58.0;
    const double a =
        (((kWinRateAs[0] * m + kWinRateAs[1]) * m + kWinRateAs[2]) * m)
        + kWinRateAs[3];
    const double b =
        (((kWinRateBs[0] * m + kWinRateBs[1]) * m + kWinRateBs[2]) * m)
        + kWinRateBs[3];

    return {a, b};
}

int to_cp(int v, const Position& pos) noexcept {
    return lookup_cp(v, win_rate_material(pos));
}

int win_rate_model(int v, const Position& pos) noexcept {
    auto [a, b] = win_rate_params(pos);

    if (std::abs(b) < 1e-9)
        return v >= static_cast<int>(a) ? 1000 : 0;

    return static_cast<int>(
        0.5 + 1000.0 / (1.0 + std::exp((a - static_cast<double>(v)) / b))
    );
}

int search_score(int v, const Position& pos) noexcept {
    return to_cp(v, pos);
}

int search_score_to_winrate(int score, const Position& pos) noexcept {
    auto [a, b] = win_rate_params(pos);

    if (std::abs(a) < 1e-9 || std::abs(b) < 1e-9)
        return score >= 0 ? 1000 : 0;

    const double raw = (static_cast<double>(score) * a) / 100.0;
    return std::clamp(win_rate_model(static_cast<int>(std::round(raw)), pos), 0, 1000);
}

WdlTriplet search_score_to_wdl(int score, const Position& pos) noexcept {
    const int win = search_score_to_winrate(score, pos);
    return {
        .win = win,
        .draw = 0,
        .loss = 1000 - win
    };
}

int search_score_to_cp(int score, const Position& pos) noexcept {
    (void)pos;
    return score;
}

} // namespace valerain::nnue
