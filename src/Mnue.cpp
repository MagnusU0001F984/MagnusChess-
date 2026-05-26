/*
MIT License

Copyright (c) 2026 Magnus

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

#include "Mnue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

namespace magnus::mnue {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u32 = std::uint32_t;

constexpr u32 kMnueMagic = 0x45554E4Du; // "MNUE" in little-endian file bytes
constexpr u32 kMnueVersion = 1;
constexpr int kQa = 255;
constexpr int kQb = 64;
constexpr int kScale = 400;
constexpr int kClip = 255;
constexpr int kMnueMaterialMax = 78;
constexpr int kMnueCpMaxRaw = 32768;

template<class Layout>
struct Network {
    bool loaded = false;
    int scale = kScale;
    std::string path;
    std::string error;

    // Row-major by feature: w0[feature][hidden]. This matches the existing
    // Chess768 loader/inference convention and keeps feature delta contiguous.
    std::vector<i16> w0;
    std::vector<i16> b0;

    // w1[bucket][perspective][hidden], perspective 0 = stm/us, 1 = nstm/them.
    std::vector<i16> w1;
    std::vector<i16> b1;
    int w1_max_abs = 0;
    bool forward_i32_safe = false;

    [[nodiscard]] bool valid() const noexcept {
        return w0.size() == static_cast<std::size_t>(Layout::InputSize) * Layout::HiddenSize
            && b0.size() == static_cast<std::size_t>(Layout::HiddenSize)
            && w1.size() == static_cast<std::size_t>(Layout::OutputBuckets) * 2 * Layout::HiddenSize
            && b1.size() == static_cast<std::size_t>(Layout::OutputBuckets);
    }
};

struct FileHeader {
    u32 magic;
    u32 version;
    u32 arch;
    u32 input_size;
    u32 hidden_size;
    u32 input_buckets;
    u32 output_buckets;
    i32 scale;
    i32 qa;
    i32 qb;
};

Network<P2Layout> g_p2;
Network<P4Layout> g_p4;
std::string g_error;
std::atomic<u32> g_p2_generation{1};

template<typename T>
[[nodiscard]] bool read_exact(std::istream& in, T* data, std::size_t count) {
    if (count == 0)
        return true;
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(sizeof(T) * count));
    return static_cast<bool>(in);
}

template<class Layout>
[[nodiscard]] constexpr std::uintmax_t payload_bytes() noexcept {
    return static_cast<std::uintmax_t>(
        static_cast<std::size_t>(Layout::InputSize) * Layout::HiddenSize +
        static_cast<std::size_t>(Layout::HiddenSize) +
        static_cast<std::size_t>(Layout::OutputBuckets) * 2 * Layout::HiddenSize +
        static_cast<std::size_t>(Layout::OutputBuckets)
    ) * sizeof(i16);
}

template<class Layout>
void clear_network(Network<Layout>& net) noexcept {
    net.loaded = false;
    net.scale = kScale;
    net.path.clear();
    net.error.clear();
    net.w0.clear();
    net.b0.clear();
    net.w1.clear();
    net.b1.clear();
    net.w1_max_abs = 0;
    net.forward_i32_safe = false;
}

template<class Layout>
void resize_network(Network<Layout>& net) {
    net.w0.resize(static_cast<std::size_t>(Layout::InputSize) * Layout::HiddenSize);
    net.b0.resize(static_cast<std::size_t>(Layout::HiddenSize));
    net.w1.resize(static_cast<std::size_t>(Layout::OutputBuckets) * 2 * Layout::HiddenSize);
    net.b1.resize(static_cast<std::size_t>(Layout::OutputBuckets));
}

template<class Layout>
[[nodiscard]] constexpr int max_safe_i32_w1_abs() noexcept {
    constexpr i64 lane_terms = (Layout::HiddenSize + 7) / 8;
    constexpr i64 max_lane_sum_per_weight =
        static_cast<i64>(kClip) * static_cast<i64>(kClip) * lane_terms;
    return static_cast<int>(
        static_cast<i64>(std::numeric_limits<i32>::max()) / max_lane_sum_per_weight
    );
}

template<class Layout>
void refresh_network_traits(Network<Layout>& net) noexcept {
    int max_abs = 0;
    for (const i16 w : net.w1) {
        const int abs_w = w < 0 ? -static_cast<int>(w) : static_cast<int>(w);
        max_abs = std::max(max_abs, abs_w);
    }

    net.w1_max_abs = max_abs;
    net.forward_i32_safe = max_abs <= max_safe_i32_w1_abs<Layout>();
}

template<class Layout>
[[nodiscard]] bool read_network_payload(std::istream& in, Network<Layout>& net) {
    resize_network(net);

    if (!read_exact(in, net.w0.data(), net.w0.size()) ||
        !read_exact(in, net.b0.data(), net.b0.size()) ||
        !read_exact(in, net.w1.data(), net.w1.size()) ||
        !read_exact(in, net.b1.data(), net.b1.size())) {
        clear_network(net);
        return false;
    }

    refresh_network_traits(net);
    return net.valid();
}


[[nodiscard]] u32 next_p2_generation() noexcept {
    u32 next = g_p2_generation.load(std::memory_order_relaxed) + 1;
    if (next == 0)
        next = 1;
    g_p2_generation.store(next, std::memory_order_release);
    return next;
}

inline void invalidate_p2_accumulator(Position& pos) noexcept {
    pos.mnue_p2_acc_valid_mask = 0;
    pos.mnue_p2_generation = g_p2_generation.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint8_t p2_perspective_mask(Color perspective) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(perspective));
}

[[nodiscard]] inline std::uint8_t p2_all_perspectives_mask() noexcept {
    return static_cast<std::uint8_t>(
        p2_perspective_mask(WHITE) | p2_perspective_mask(BLACK)
    );
}

[[nodiscard]] inline bool p2_generation_matches(const Position& pos) noexcept {
    return pos.mnue_p2_generation == g_p2_generation.load(std::memory_order_relaxed);
}

inline void sync_p2_generation(const Position& pos) noexcept {
    const u32 current = g_p2_generation.load(std::memory_order_relaxed);
    if (pos.mnue_p2_generation != current) {
        pos.mnue_p2_generation = current;
        pos.mnue_p2_acc_valid_mask = 0;
    }
}

inline void invalidate_p2_perspective(Position& pos, Color perspective) noexcept {
    sync_p2_generation(pos);
    pos.mnue_p2_acc_valid_mask = static_cast<std::uint8_t>(
        pos.mnue_p2_acc_valid_mask & ~p2_perspective_mask(perspective)
    );
}

[[nodiscard]] inline bool p2_accumulator_matches(
    const Position& pos,
    Color perspective
) noexcept {
    return p2_generation_matches(pos)
        && ((pos.mnue_p2_acc_valid_mask & p2_perspective_mask(perspective)) != 0);
}

[[nodiscard]] inline bool p2_accumulators_match(const Position& pos) noexcept {
    return p2_generation_matches(pos)
        && ((pos.mnue_p2_acc_valid_mask & p2_all_perspectives_mask())
            == p2_all_perspectives_mask());
}


template<class Layout>
[[nodiscard]] bool load_network(Network<Layout>& net, const std::string& path) {
    clear_network(net);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        net.error = "could not open MNUE file: " + path;
        g_error = net.error;
        return false;
    }

    FileHeader header{};
    if (!read_exact(in, &header, 1)) {
        net.error = "could not read MNUE header";
        g_error = net.error;
        return false;
    }

    if (header.magic != kMnueMagic || header.version != kMnueVersion) {
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
        if (!ec && bytes == payload_bytes<Layout>()) {
            in.clear();
            in.seekg(0, std::ios::beg);
            if (read_network_payload(in, net)) {
                net.loaded = true;
                net.path = path;
                g_error.clear();
                return true;
            }

            net.error = "truncated headerless MNUE weight file";
            g_error = net.error;
            return false;
        }

        net.error = "bad MNUE magic/version";
        g_error = net.error;
        return false;
    }

    if (header.arch != Layout::ArchId ||
        header.input_size != Layout::InputSize ||
        header.hidden_size != Layout::HiddenSize ||
        header.input_buckets != Layout::InputBuckets ||
        header.output_buckets != Layout::OutputBuckets ||
        header.qa != kQa ||
        header.qb != kQb) {
        net.error = "MNUE header dimensions do not match requested layout";
        g_error = net.error;
        return false;
    }

    net.scale = header.scale;
    if (!read_network_payload(in, net)) {
        net.error = "truncated MNUE weight file";
        g_error = net.error;
        return false;
    }

    if (!net.valid()) {
        clear_network(net);
        net.error = "internal MNUE size check failed";
        g_error = net.error;
        return false;
    }

    net.loaded = true;
    net.path = path;
    g_error.clear();
    return true;
}

[[nodiscard]] inline Square flip_vertical_sq(Square sq) noexcept {
    return static_cast<Square>(static_cast<int>(sq) ^ 56);
}

[[nodiscard]] inline Square relative_square(Color perspective, Square sq) noexcept {
    return perspective == WHITE ? sq : flip_vertical_sq(sq);
}

[[nodiscard]] inline int file_of(Square sq) noexcept {
    return static_cast<int>(sq) & 7;
}

[[nodiscard]] inline int rank_of(Square sq) noexcept {
    return static_cast<int>(sq) >> 3;
}

[[nodiscard]] inline Square king_square_of(const Position& pos, Color color) noexcept {
    const Bitboard bb = pieces(pos, color, KING);
    if (bb == 0)
        return static_cast<Square>(0);
    return static_cast<Square>(std::countr_zero(static_cast<std::uint64_t>(bb)));
}

[[nodiscard]] inline int nonking_piece_index(PieceType pt) noexcept {
    switch (pt) {
        case PAWN:   return 0;
        case KNIGHT: return 1;
        case BISHOP: return 2;
        case ROOK:   return 3;
        case QUEEN:  return 4;
        default:     return -1;
    }
}

[[nodiscard]] inline int king_zone16(Square relative_king_sq) noexcept {
    const int file_group = file_of(relative_king_sq) / 2; // 0..3
    const int rank_group = rank_of(relative_king_sq) / 2; // 0..3
    return rank_group * 4 + file_group;                  // 0..15
}

[[nodiscard]] inline int king_zone32(Square relative_king_sq) noexcept {
    const int file_group = file_of(relative_king_sq);      // 0..7
    const int rank_group = rank_of(relative_king_sq) / 2;  // 0..3
    return rank_group * 8 + file_group;                   // 0..31
}

[[nodiscard]] inline int phase_bucket2(const Position& pos) noexcept {
    // Must match the Bullet trainer exactly:
    //   knight/bishop = 1, rook = 2, queen = 4, pawns ignored.
    //
    // Trainer:
    //   let mut npm = 0;
    //   N/B += 1, R += 2, Q += 4;
    //   phase = usize::from(npm < 10);
    //
    // Therefore:
    //   phase 0 = opening/middlegame
    //   phase 1 = simplified/endgame
    const int npm =
        piece_count(pos, WHITE, KNIGHT) + piece_count(pos, BLACK, KNIGHT) +
        piece_count(pos, WHITE, BISHOP) + piece_count(pos, BLACK, BISHOP) +
        2 * (piece_count(pos, WHITE, ROOK) + piece_count(pos, BLACK, ROOK)) +
        4 * (piece_count(pos, WHITE, QUEEN) + piece_count(pos, BLACK, QUEEN));

    return npm < 10 ? 1 : 0;
}

[[nodiscard]] inline int game_ply(const Position& pos) noexcept {
    const int fullmove = std::max(1, pos.fullmove_number);
    return (fullmove - 1) * 2 + (pos.side_to_move == BLACK ? 1 : 0);
}

[[nodiscard]] inline int mnue_material_units(const Position& pos) noexcept {
    return std::clamp(non_king_material(pos), 0, kMnueMaterialMax);
}

[[nodiscard]] inline double mnue_material_ratio(const Position& pos) noexcept {
    return static_cast<double>(mnue_material_units(pos))
        / static_cast<double>(kMnueMaterialMax);
}

[[nodiscard]] inline double mnue_cp_denominator(const Position& pos) noexcept {
    const double material = mnue_material_ratio(pos);
    double denom = 320.0 + 70.0 * material;
    if (phase_bucket2(pos) != 0)
        denom -= 10.0;
    return std::clamp(denom, 280.0, 410.0);
}

[[nodiscard]] inline int mnue_to_cp(int raw, const Position& pos) noexcept {
    const i64 abs_raw = raw >= 0 ? static_cast<i64>(raw) : -static_cast<i64>(raw);
    const double clipped = static_cast<double>(
        std::min<i64>(abs_raw, kMnueCpMaxRaw)
    );
    const int cp = static_cast<int>(
        std::round(clipped * 100.0 / mnue_cp_denominator(pos))
    );
    return raw >= 0 ? cp : -cp;
}

[[nodiscard]] inline WinRateParams mnue_win_rate_params(const Position& pos) noexcept {
    const double material = mnue_material_ratio(pos);
    const double ply = static_cast<double>(std::min(240, game_ply(pos))) / 240.0;
    const double low_material = 1.0 - material;

    double a = 138.0 + 36.0 * material + 8.0 * ply;
    double b = 54.0 + 32.0 * material + 6.0 * ply;

    if (mnue_material_units(pos) <= 4) {
        a += 130.0 * low_material;
        b += 22.0 * low_material;
    }

    return {a, b};
}

[[nodiscard]] inline WdlTriplet mnue_wdl_from_cp(
    int cp,
    const Position& pos
) noexcept {
    const auto [a, b] = mnue_win_rate_params(pos);
    const double x = std::clamp(static_cast<double>(cp), -2400.0, 2400.0);
    const double slope = std::max(1.0, b);

    const double win = 1.0 / (1.0 + std::exp((a - x) / slope));
    const double loss = 1.0 / (1.0 + std::exp((a + x) / slope));

    int win_i = std::clamp(static_cast<int>(std::round(1000.0 * win)), 0, 1000);
    int loss_i = std::clamp(static_cast<int>(std::round(1000.0 * loss)), 0, 1000);
    if (win_i + loss_i > 1000) {
        const int total = win_i + loss_i;
        win_i = (win_i * 1000) / total;
        loss_i = 1000 - win_i;
    }

    return {
        .win = win_i,
        .draw = 1000 - win_i - loss_i,
        .loss = loss_i
    };
}

template<class Layout>
[[nodiscard]] inline int input_bucket(const Position& pos, Color perspective) noexcept {
    const Square rel_ksq = relative_square(perspective, king_square_of(pos, perspective));
    if constexpr (Layout::InputBuckets == 16) {
        return king_zone16(rel_ksq);
    } else {
        return king_zone32(rel_ksq);
    }
}

template<class Layout>
[[nodiscard]] inline int output_bucket(const Position& pos, Color stm) noexcept {
    const Square rel_ksq = relative_square(stm, king_square_of(pos, stm));
    return phase_bucket2(pos) * 16 + king_zone16(rel_ksq); // 0..31
}

template<class Layout>
[[nodiscard]] inline int feature_index(
    const Position& pos,
    Color perspective,
    Piece pc,
    Square sq
) noexcept {
    const PieceType pt = type_of(pc);
    const int pt_idx = nonking_piece_index(pt);
    if (pt_idx < 0)
        return -1;

    const int bucket = input_bucket<Layout>(pos, perspective);
    const int rel_color = color_of(pc) == perspective ? 0 : 1;
    const int rel_sq = static_cast<int>(relative_square(perspective, sq));

    return (((bucket * 2 + rel_color) * 5 + pt_idx) * 64 + rel_sq);
}

template<class Layout>
using Accumulator = std::array<i16, Layout::HiddenSize>;


#if defined(__AVX2__)
template<class Layout>
inline void add_feature_vector_avx2(
    std::array<i16, Layout::HiddenSize>& acc,
    const i16* w
) noexcept {
    i16* acc_ptr = acc.data();
    for (int i = 0; i < Layout::HiddenSize; i += 16) {
        const __m256i a = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(acc_ptr + i)
        );
        const __m256i b = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(w + i)
        );
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(acc_ptr + i),
            _mm256_add_epi16(a, b)
        );
    }
}

template<class Layout>
inline void sub_feature_vector_avx2(
    std::array<i16, Layout::HiddenSize>& acc,
    const i16* w
) noexcept {
    i16* acc_ptr = acc.data();
    for (int i = 0; i < Layout::HiddenSize; i += 16) {
        const __m256i a = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(acc_ptr + i)
        );
        const __m256i b = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(w + i)
        );
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(acc_ptr + i),
            _mm256_sub_epi16(a, b)
        );
    }
}

template<class Layout>
inline void move_feature_vector_avx2(
    std::array<i16, Layout::HiddenSize>& acc,
    const i16* add,
    const i16* sub
) noexcept {
    i16* acc_ptr = acc.data();
    for (int i = 0; i < Layout::HiddenSize; i += 16) {
        const __m256i a = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(acc_ptr + i)
        );
        const __m256i add_v = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(add + i)
        );
        const __m256i sub_v = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(sub + i)
        );
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(acc_ptr + i),
            _mm256_add_epi16(a, _mm256_sub_epi16(add_v, sub_v))
        );
    }
}
#endif

template<class Layout>
void add_feature(
    Accumulator<Layout>& acc,
    const Network<Layout>& net,
    int idx
) noexcept {
    const i16* w = net.w0.data() + static_cast<std::size_t>(idx) * Layout::HiddenSize;
#if defined(__AVX2__)
    add_feature_vector_avx2<Layout>(acc, w);
#else
    for (int i = 0; i < Layout::HiddenSize; ++i)
        acc[static_cast<std::size_t>(i)] = static_cast<i16>(
            static_cast<i32>(acc[static_cast<std::size_t>(i)]) + static_cast<i32>(w[i])
        );
#endif
}


template<class Layout>
void sub_feature(
    Accumulator<Layout>& acc,
    const Network<Layout>& net,
    int idx
) noexcept {
    const i16* w = net.w0.data() + static_cast<std::size_t>(idx) * Layout::HiddenSize;
#if defined(__AVX2__)
    sub_feature_vector_avx2<Layout>(acc, w);
#else
    for (int i = 0; i < Layout::HiddenSize; ++i)
        acc[static_cast<std::size_t>(i)] = static_cast<i16>(
            static_cast<i32>(acc[static_cast<std::size_t>(i)]) - static_cast<i32>(w[i])
        );
#endif
}

template<class Layout>
void move_feature(
    Accumulator<Layout>& acc,
    const Network<Layout>& net,
    int add_idx,
    int sub_idx
) noexcept {
    const i16* add = net.w0.data() + static_cast<std::size_t>(add_idx) * Layout::HiddenSize;
    const i16* sub = net.w0.data() + static_cast<std::size_t>(sub_idx) * Layout::HiddenSize;
#if defined(__AVX2__)
    move_feature_vector_avx2<Layout>(acc, add, sub);
#else
    for (int i = 0; i < Layout::HiddenSize; ++i)
        acc[static_cast<std::size_t>(i)] = static_cast<i16>(
            static_cast<i32>(acc[static_cast<std::size_t>(i)])
            + static_cast<i32>(add[i])
            - static_cast<i32>(sub[i])
        );
#endif
}

template<class Layout>
void rebuild_accumulator(
    const Position& pos,
    Color perspective,
    Accumulator<Layout>& acc,
    const Network<Layout>& net
) noexcept {
    std::copy(net.b0.begin(), net.b0.end(), acc.begin());

    Bitboard bb = pieces(pos);
    while (bb) {
        const Square sq = static_cast<Square>(
            std::countr_zero(static_cast<std::uint64_t>(bb))
        );
        bb &= (bb - 1);

        const Piece pc = piece_on(pos, sq);
        if (pc == PIECE_NONE)
            continue;

        const int idx = feature_index<Layout>(pos, perspective, pc, sq);
        if (idx >= 0)
            add_feature(acc, net, idx);
    }
}

[[nodiscard]] inline i32 screlu(i32 x) noexcept {
    const i32 y = std::clamp(x, 0, kClip);
    return y * y;
}


#if defined(__AVX2__)
[[nodiscard]] inline i64 horizontal_sum_epi64_avx2(__m256i v) noexcept {
    const __m128i lo = _mm256_castsi256_si128(v);
    const __m128i hi = _mm256_extracti128_si256(v, 1);
    const __m128i sum = _mm_add_epi64(lo, hi);
    return static_cast<i64>(_mm_cvtsi128_si64(sum))
        + static_cast<i64>(_mm_extract_epi64(sum, 1));
}

[[nodiscard]] inline i64 horizontal_sum_epi32_to_i64_avx2(__m256i v) noexcept {
    const __m128i lo = _mm256_castsi256_si128(v);
    const __m128i hi = _mm256_extracti128_si256(v, 1);
    const __m256i lo64 = _mm256_cvtepi32_epi64(lo);
    const __m256i hi64 = _mm256_cvtepi32_epi64(hi);
    return horizontal_sum_epi64_avx2(_mm256_add_epi64(lo64, hi64));
}

[[nodiscard]] inline __m256i cvt_i32x4_to_i64x4_avx2(__m128i v) noexcept {
    return _mm256_cvtepi32_epi64(v);
}

[[nodiscard]] i64 dot_screlu_i16_i32_avx2(
    const i16* acc,
    const i16* weights,
    int count
) noexcept {
    const __m256i zero16 = _mm256_setzero_si256();
    const __m256i clip16 = _mm256_set1_epi16(static_cast<i16>(kClip));
    __m256i sum32 = _mm256_setzero_si256();

    int i = 0;
    for (; i + 15 < count; i += 16) {
        const __m256i acc16_raw = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(acc + i)
        );
        const __m256i w16_raw = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(weights + i)
        );

        const __m256i clipped16 = _mm256_min_epi16(
            _mm256_max_epi16(acc16_raw, zero16),
            clip16
        );

        const __m128i acc_lo16 = _mm256_castsi256_si128(clipped16);
        const __m128i acc_hi16 = _mm256_extracti128_si256(clipped16, 1);
        const __m128i w_lo16 = _mm256_castsi256_si128(w16_raw);
        const __m128i w_hi16 = _mm256_extracti128_si256(w16_raw, 1);

        const __m256i acc_lo32 = _mm256_cvtepi16_epi32(acc_lo16);
        const __m256i acc_hi32 = _mm256_cvtepi16_epi32(acc_hi16);
        const __m256i w_lo32 = _mm256_cvtepi16_epi32(w_lo16);
        const __m256i w_hi32 = _mm256_cvtepi16_epi32(w_hi16);

        const __m256i sq_lo32 = _mm256_mullo_epi32(acc_lo32, acc_lo32);
        const __m256i sq_hi32 = _mm256_mullo_epi32(acc_hi32, acc_hi32);

        sum32 = _mm256_add_epi32(
            sum32,
            _mm256_mullo_epi32(sq_lo32, w_lo32)
        );
        sum32 = _mm256_add_epi32(
            sum32,
            _mm256_mullo_epi32(sq_hi32, w_hi32)
        );
    }

    i64 total = horizontal_sum_epi32_to_i64_avx2(sum32);
    for (; i < count; ++i)
        total += static_cast<i64>(screlu(acc[i])) * static_cast<i64>(weights[i]);

    return total;
}

[[nodiscard]] i64 dot_screlu_i16_avx2(
    const i16* acc,
    const i16* weights,
    int count
) noexcept {
    const __m256i zero16 = _mm256_setzero_si256();
    const __m256i clip16 = _mm256_set1_epi16(static_cast<i16>(kClip));
    __m256i sum64 = _mm256_setzero_si256();

    int i = 0;
    for (; i + 15 < count; i += 16) {
        const __m256i acc16_raw = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(acc + i)
        );
        const __m256i w16_raw = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(weights + i)
        );

        const __m256i clipped16 = _mm256_min_epi16(
            _mm256_max_epi16(acc16_raw, zero16),
            clip16
        );

        const __m128i acc_lo16 = _mm256_castsi256_si128(clipped16);
        const __m128i acc_hi16 = _mm256_extracti128_si256(clipped16, 1);
        const __m128i w_lo16 = _mm256_castsi256_si128(w16_raw);
        const __m128i w_hi16 = _mm256_extracti128_si256(w16_raw, 1);

        const __m256i acc_lo32 = _mm256_cvtepi16_epi32(acc_lo16);
        const __m256i acc_hi32 = _mm256_cvtepi16_epi32(acc_hi16);
        const __m256i w_lo32 = _mm256_cvtepi16_epi32(w_lo16);
        const __m256i w_hi32 = _mm256_cvtepi16_epi32(w_hi16);

        const __m256i sq_lo32 = _mm256_mullo_epi32(acc_lo32, acc_lo32);
        const __m256i sq_hi32 = _mm256_mullo_epi32(acc_hi32, acc_hi32);
        const __m256i prod_lo32 = _mm256_mullo_epi32(sq_lo32, w_lo32);
        const __m256i prod_hi32 = _mm256_mullo_epi32(sq_hi32, w_hi32);

        sum64 = _mm256_add_epi64(
            sum64,
            cvt_i32x4_to_i64x4_avx2(_mm256_castsi256_si128(prod_lo32))
        );
        sum64 = _mm256_add_epi64(
            sum64,
            cvt_i32x4_to_i64x4_avx2(_mm256_extracti128_si256(prod_lo32, 1))
        );
        sum64 = _mm256_add_epi64(
            sum64,
            cvt_i32x4_to_i64x4_avx2(_mm256_castsi256_si128(prod_hi32))
        );
        sum64 = _mm256_add_epi64(
            sum64,
            cvt_i32x4_to_i64x4_avx2(_mm256_extracti128_si256(prod_hi32, 1))
        );
    }

    i64 total = horizontal_sum_epi64_avx2(sum64);
    for (; i < count; ++i)
        total += static_cast<i64>(screlu(acc[i])) * static_cast<i64>(weights[i]);

    return total;
}
#endif

template<class Layout>
[[nodiscard]] int forward(
    const Position& pos,
    const Network<Layout>& net,
    const Accumulator<Layout>& stm_acc,
    const Accumulator<Layout>& nstm_acc
) noexcept {
    const Color stm = static_cast<Color>(pos.side_to_move);
    const int bucket = output_bucket<Layout>(pos, stm);
    const std::size_t base = static_cast<std::size_t>(bucket) * 2 * Layout::HiddenSize;
    const i16* w_stm = net.w1.data() + base;
    const i16* w_nstm = w_stm + Layout::HiddenSize;

    i64 output = 0;
#if defined(__AVX2__)
    if (net.forward_i32_safe) {
        output += dot_screlu_i16_i32_avx2(stm_acc.data(), w_stm, Layout::HiddenSize);
        output += dot_screlu_i16_i32_avx2(nstm_acc.data(), w_nstm, Layout::HiddenSize);
    } else {
        output += dot_screlu_i16_avx2(stm_acc.data(), w_stm, Layout::HiddenSize);
        output += dot_screlu_i16_avx2(nstm_acc.data(), w_nstm, Layout::HiddenSize);
    }
#else
    for (int i = 0; i < Layout::HiddenSize; ++i)
        output += static_cast<i64>(screlu(stm_acc[static_cast<std::size_t>(i)]))
            * static_cast<i64>(w_stm[i]);

    for (int i = 0; i < Layout::HiddenSize; ++i)
        output += static_cast<i64>(screlu(nstm_acc[static_cast<std::size_t>(i)]))
            * static_cast<i64>(w_nstm[i]);
#endif

    output /= kQa;
    output += static_cast<i64>(net.b1[static_cast<std::size_t>(bucket)]);
    output *= net.scale;
    output /= static_cast<i64>(kQa) * kQb;

    return static_cast<int>(std::clamp<i64>(output, -32000, 32000));
}

template<class Layout>
[[nodiscard]] int forward_reference(
    const Position& pos,
    const Network<Layout>& net,
    const Accumulator<Layout>& stm_acc,
    const Accumulator<Layout>& nstm_acc
) noexcept {
    const Color stm = static_cast<Color>(pos.side_to_move);
    const int bucket = output_bucket<Layout>(pos, stm);
    const std::size_t base = static_cast<std::size_t>(bucket) * 2 * Layout::HiddenSize;
    const i16* w_stm = net.w1.data() + base;
    const i16* w_nstm = w_stm + Layout::HiddenSize;

    i64 output = 0;
    for (int i = 0; i < Layout::HiddenSize; ++i)
        output += static_cast<i64>(screlu(stm_acc[static_cast<std::size_t>(i)]))
            * static_cast<i64>(w_stm[i]);

    for (int i = 0; i < Layout::HiddenSize; ++i)
        output += static_cast<i64>(screlu(nstm_acc[static_cast<std::size_t>(i)]))
            * static_cast<i64>(w_nstm[i]);

    output /= kQa;
    output += static_cast<i64>(net.b1[static_cast<std::size_t>(bucket)]);
    output *= net.scale;
    output /= static_cast<i64>(kQa) * kQb;

    return static_cast<int>(std::clamp<i64>(output, -32000, 32000));
}

template<class Layout>
[[nodiscard]] int eval_lazy(
    const Position& pos,
    const Network<Layout>& net
) noexcept {
    if (!net.loaded || !net.valid())
        return 0;

    thread_local Accumulator<Layout> white_acc{};
    thread_local Accumulator<Layout> black_acc{};

    rebuild_accumulator<Layout>(pos, WHITE, white_acc, net);
    rebuild_accumulator<Layout>(pos, BLACK, black_acc, net);

    const Color stm = static_cast<Color>(pos.side_to_move);
    const auto& stm_acc = stm == WHITE ? white_acc : black_acc;
    const auto& nstm_acc = stm == WHITE ? black_acc : white_acc;
    return forward<Layout>(pos, net, stm_acc, nstm_acc);
}


void rebuild_p2_accumulator(const Position& pos, Color perspective) noexcept {
    if (!g_p2.loaded || !g_p2.valid())
        return;

    sync_p2_generation(pos);
    rebuild_accumulator<P2Layout>(
        pos,
        perspective,
        pos.mnue_p2_acc[perspective],
        g_p2
    );

    pos.mnue_p2_acc_valid_mask = static_cast<std::uint8_t>(
        pos.mnue_p2_acc_valid_mask | p2_perspective_mask(perspective)
    );
}

inline void ensure_p2_accumulator(const Position& pos, Color perspective) noexcept {
    if (!p2_accumulator_matches(pos, perspective))
        rebuild_p2_accumulator(pos, perspective);
}

inline void ensure_p2_accumulators(const Position& pos) noexcept {
    ensure_p2_accumulator(pos, WHITE);
    ensure_p2_accumulator(pos, BLACK);
}

[[nodiscard]] int eval_p2_incremental(const Position& pos) noexcept {
    if (!g_p2.loaded || !g_p2.valid())
        return 0;

    ensure_p2_accumulators(pos);

    const Color stm = static_cast<Color>(pos.side_to_move);
    const auto& stm_acc = stm == WHITE ? pos.mnue_p2_acc[WHITE] : pos.mnue_p2_acc[BLACK];
    const auto& nstm_acc = stm == WHITE ? pos.mnue_p2_acc[BLACK] : pos.mnue_p2_acc[WHITE];

    return forward<P2Layout>(pos, g_p2, stm_acc, nstm_acc);
}

void apply_p2_piece_delta(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq,
    int delta
) noexcept {
    if (!g_p2.loaded || !g_p2.valid() || !p2_generation_matches(pos))
        return;

    if (piece_type == KING) {
        invalidate_p2_perspective(pos, color);
        return;
    }

    const Piece pc = make_piece(color, piece_type);
    for (int persp = WHITE; persp <= BLACK; ++persp) {
        const Color perspective = static_cast<Color>(persp);
        if (!p2_accumulator_matches(pos, perspective))
            continue;

        const int idx = feature_index<P2Layout>(pos, perspective, pc, sq);
        if (idx < 0)
            continue;

        if (delta > 0)
            add_feature<P2Layout>(pos.mnue_p2_acc[persp], g_p2, idx);
        else
            sub_feature<P2Layout>(pos.mnue_p2_acc[persp], g_p2, idx);
    }
}

void apply_p2_piece_move_delta(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    if (!g_p2.loaded || !g_p2.valid() || !p2_generation_matches(pos))
        return;

    if (piece_type == KING) {
        const int from_bucket = king_zone16(relative_square(color, from));
        const int to_bucket = king_zone16(relative_square(color, to));
        if (from_bucket != to_bucket)
            invalidate_p2_perspective(pos, color);
        return;
    }

    const Piece pc = make_piece(color, piece_type);
    for (int persp = WHITE; persp <= BLACK; ++persp) {
        const Color perspective = static_cast<Color>(persp);
        if (!p2_accumulator_matches(pos, perspective))
            continue;

        const int sub_idx = feature_index<P2Layout>(pos, perspective, pc, from);
        const int add_idx = feature_index<P2Layout>(pos, perspective, pc, to);

        if (sub_idx >= 0 && add_idx >= 0) {
            move_feature<P2Layout>(pos.mnue_p2_acc[persp], g_p2, add_idx, sub_idx);
            continue;
        }

        if (sub_idx >= 0)
            sub_feature<P2Layout>(pos.mnue_p2_acc[persp], g_p2, sub_idx);
        if (add_idx >= 0)
            add_feature<P2Layout>(pos.mnue_p2_acc[persp], g_p2, add_idx);
    }
}

} // namespace

bool load_p2(const std::string& path) {
    const bool ok = load_network(g_p2, path);
    if (ok)
        (void)next_p2_generation();
    return ok;
}

bool load_p4(const std::string& path) {
    return load_network(g_p4, path);
}

void unload_p2() noexcept {
    clear_network(g_p2);
    (void)next_p2_generation();
}

void unload_p4() noexcept {
    clear_network(g_p4);
}

void unload_all() noexcept {
    unload_p2();
    unload_p4();
}

bool p2_loaded() noexcept {
    return g_p2.loaded;
}

bool p4_loaded() noexcept {
    return g_p4.loaded;
}

const std::string& p2_path() noexcept {
    return g_p2.path;
}

const std::string& p4_path() noexcept {
    return g_p4.path;
}

const std::string& last_error() noexcept {
    return g_error;
}

int eval_p2(const Position& pos) noexcept {
    return eval_p2_incremental(pos);
}

int debug_eval_p2_reference(const Position& pos) noexcept {
    if (!g_p2.loaded || !g_p2.valid())
        return 0;

    ensure_p2_accumulators(pos);

    const Color stm = static_cast<Color>(pos.side_to_move);
    const auto& stm_acc = stm == WHITE ? pos.mnue_p2_acc[WHITE] : pos.mnue_p2_acc[BLACK];
    const auto& nstm_acc = stm == WHITE ? pos.mnue_p2_acc[BLACK] : pos.mnue_p2_acc[WHITE];

    return forward_reference<P2Layout>(pos, g_p2, stm_acc, nstm_acc);
}

bool p2_i32_forward_enabled() noexcept {
#if defined(__AVX2__)
    return g_p2.loaded && g_p2.valid() && g_p2.forward_i32_safe;
#else
    return false;
#endif
}

int p2_w1_max_abs() noexcept {
    return g_p2.loaded && g_p2.valid() ? g_p2.w1_max_abs : 0;
}

int eval_p4_lazy(const Position& pos) noexcept {
    return eval_lazy(pos, g_p4);
}


void debug_dump_p2_features(const Position& pos, std::ostream& out) {
    out << "mnue debug p2\n";
    out << "side_to_move " << pos.side_to_move << '\n';

    const int out_bucket = output_bucket<P2Layout>(
        pos,
        static_cast<Color>(pos.side_to_move)
    );
    out << "output_bucket " << out_bucket << '\n';

    if (g_p2.loaded && g_p2.valid()) {
        out << "p2_loaded 1\n";
        out << "p2_eval_raw " << eval_p2(pos) << '\n';
    } else {
        out << "p2_loaded 0\n";
    }

    for (int persp = WHITE; persp <= BLACK; ++persp) {
        const Color perspective = static_cast<Color>(persp);
        const Square ksq = king_square_of(pos, perspective);
        const Square rksq = relative_square(perspective, ksq);
        const int bucket = input_bucket<P2Layout>(pos, perspective);

        out << "perspective " << persp
            << " king_sq " << static_cast<int>(ksq)
            << " rel_king_sq " << static_cast<int>(rksq)
            << " input_bucket " << bucket
            << '\n';

        Bitboard bb = pieces(pos);
        while (bb) {
            const Square sq = static_cast<Square>(
                std::countr_zero(static_cast<std::uint64_t>(bb))
            );
            bb &= (bb - 1);

            const Piece pc = piece_on(pos, sq);
            if (pc == PIECE_NONE)
                continue;

            const int idx = feature_index<P2Layout>(pos, perspective, pc, sq);
            if (idx < 0)
                continue;

            out << "  feature"
                << " sq " << static_cast<int>(sq)
                << " rel_sq " << static_cast<int>(relative_square(perspective, sq))
                << " piece " << static_cast<int>(pc)
                << " color " << static_cast<int>(color_of(pc))
                << " type " << static_cast<int>(type_of(pc))
                << " idx " << idx
                << '\n';
        }
    }
}


void on_position_cleared(Position& pos) noexcept {
    invalidate_p2_accumulator(pos);
}

void on_piece_added(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    apply_p2_piece_delta(pos, color, piece_type, sq, 1);
}

void on_piece_removed(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept {
    apply_p2_piece_delta(pos, color, piece_type, sq, -1);
}

void on_piece_moved(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept {
    apply_p2_piece_move_delta(pos, color, piece_type, from, to);
}


bool debug_check_p2_incremental(
    const Position& pos,
    std::ostream& out
) noexcept {
    if (!g_p2.loaded || !g_p2.valid()) {
        out << "mnuecheck p2 loaded 0\n";
        return false;
    }

    const std::uint8_t valid_mask_before = pos.mnue_p2_acc_valid_mask;
    const bool valid_before = p2_accumulators_match(pos);
    const u32 generation_before = pos.mnue_p2_generation;

    const int incremental = eval_p2_incremental(pos);

    Accumulator<P2Layout> white_rebuild{};
    Accumulator<P2Layout> black_rebuild{};
    rebuild_accumulator<P2Layout>(pos, WHITE, white_rebuild, g_p2);
    rebuild_accumulator<P2Layout>(pos, BLACK, black_rebuild, g_p2);

    const Color stm = static_cast<Color>(pos.side_to_move);
    const auto& rebuilt_stm = stm == WHITE ? white_rebuild : black_rebuild;
    const auto& rebuilt_nstm = stm == WHITE ? black_rebuild : white_rebuild;
    const int rebuilt = forward<P2Layout>(pos, g_p2, rebuilt_stm, rebuilt_nstm);

    const bool white_acc_equal = std::equal(
        pos.mnue_p2_acc[WHITE].begin(),
        pos.mnue_p2_acc[WHITE].end(),
        white_rebuild.begin()
    );
    const bool black_acc_equal = std::equal(
        pos.mnue_p2_acc[BLACK].begin(),
        pos.mnue_p2_acc[BLACK].end(),
        black_rebuild.begin()
    );
    const bool score_equal = incremental == rebuilt;
    const bool ok = score_equal && white_acc_equal && black_acc_equal;

    out << "mnuecheck p2 loaded 1"
        << " valid_before " << (valid_before ? 1 : 0)
        << " valid_mask_before " << static_cast<int>(valid_mask_before)
        << " generation_before " << generation_before
        << " generation_current " << g_p2_generation.load(std::memory_order_relaxed)
        << " incremental " << incremental
        << " rebuild " << rebuilt
        << " score_equal " << (score_equal ? 1 : 0)
        << " white_acc_equal " << (white_acc_equal ? 1 : 0)
        << " black_acc_equal " << (black_acc_equal ? 1 : 0)
        << " ok " << (ok ? 1 : 0)
        << '\n';

    return ok;
}

int search_score(int v, const Position& pos) noexcept {
    (void)pos;
    return v;
}

int material_units(const Position& pos) noexcept {
    return mnue_material_units(pos);
}

WinRateParams win_rate_params(const Position& pos) noexcept {
    return mnue_win_rate_params(pos);
}

int to_cp(int v, const Position& pos) noexcept {
    return mnue_to_cp(v, pos);
}

int win_rate_model(int v, const Position& pos) noexcept {
    return mnue_wdl_from_cp(to_cp(v, pos), pos).win;
}

int search_score_to_cp(int score, const Position& pos) noexcept {
    return to_cp(score, pos);
}

int search_score_to_winrate(int score, const Position& pos) noexcept {
    return search_score_to_wdl(score, pos).win;
}

WdlTriplet search_score_to_wdl(int score, const Position& pos) noexcept {
    return mnue_wdl_from_cp(search_score_to_cp(score, pos), pos);
}

} // namespace magnus::mnue
