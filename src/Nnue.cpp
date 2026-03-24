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
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "Types.h"

namespace valerain::nnue {
namespace {

using std::int8_t;
using std::int16_t;
using std::int32_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

constexpr uint32_t kVersion = 0x7AF32F20u;
constexpr int kOutputScale = 16;
constexpr int kWeightScaleBits = 6;

constexpr int kTransformedDims = 128;
constexpr int kFeatureDims = 22528;
constexpr int kBuckets = 8;

constexpr int kL2 = 15;                       // Stockfish L2Small
constexpr int kL3 = 32;                       // Stockfish L3Small
constexpr int kFc0LayerOutputs = kL2 + 1;     // 16, includes forward channel
constexpr int kFc1LayerInputs = kL2 * 2;      // 30
constexpr int kFc1PaddedInputs = 32;          // ceil_to_multiple(30, 32)
constexpr int kFc2Inputs = kL3;               // 32

constexpr const char kLebMagic[] = "COMPRESSED_LEB128";
constexpr std::size_t kLebMagicSize = sizeof(kLebMagic) - 1;

constexpr uint32_t affine_hash(uint32_t prev, uint32_t outdim) noexcept {
    uint32_t h = 0xCC03DAE4u;
    h += outdim;
    h ^= prev >> 1;
    h ^= prev << 31;
    return h;
}

constexpr uint32_t relu_hash(uint32_t prev) noexcept {
    return 0x538D24C7u + prev;
}

constexpr uint32_t transformer_hash() noexcept {
    return 0x7f234cb8u ^ (kTransformedDims * 2u);
}

constexpr uint32_t bucket_arch_hash() noexcept {
    uint32_t h = 0xEC42E90Du;
    h ^= (kTransformedDims * 2u);
    h = affine_hash(h, static_cast<uint32_t>(kFc0LayerOutputs)); // fc_0 : 16 outputs
    h = relu_hash(h);                                            // ac_0 only, NOT ac_sqr_0
    h = affine_hash(h, static_cast<uint32_t>(kL3));              // fc_1 : 32 outputs
    h = relu_hash(h);                                            // ac_1
    h = affine_hash(h, 1u);                                      // fc_2 : 1 output
    return h;
}

constexpr uint32_t network_hash() noexcept {
    return transformer_hash() ^ bucket_arch_hash();
}

constexpr int piece_channel(Color perspective, Piece pc) noexcept {
    if (pc == PIECE_NONE) return 0;
    const PieceType pt = type_of(pc);
    if (pt == KING) return 10 * 64;
    const int foe = (color_of(pc) == perspective) ? 0 : 1;
    return (2 * static_cast<int>(pt) + foe) * 64;
}

constexpr std::array<int, 64> king_buckets = {
    28 * 704, 29 * 704, 30 * 704, 31 * 704, 31 * 704, 30 * 704, 29 * 704, 28 * 704,
    24 * 704, 25 * 704, 26 * 704, 27 * 704, 27 * 704, 26 * 704, 25 * 704, 24 * 704,
    20 * 704, 21 * 704, 22 * 704, 23 * 704, 23 * 704, 22 * 704, 21 * 704, 20 * 704,
    16 * 704, 17 * 704, 18 * 704, 19 * 704, 19 * 704, 18 * 704, 17 * 704, 16 * 704,
    12 * 704, 13 * 704, 14 * 704, 15 * 704, 15 * 704, 14 * 704, 13 * 704, 12 * 704,
     8 * 704,  9 * 704, 10 * 704, 11 * 704, 11 * 704, 10 * 704,  9 * 704,  8 * 704,
     4 * 704,  5 * 704,  6 * 704,  7 * 704,  7 * 704,  6 * 704,  5 * 704,  4 * 704,
     0 * 704,  1 * 704,  2 * 704,  3 * 704,  3 * 704,  2 * 704,  1 * 704,  0 * 704,
};

struct SmallBucket {
    std::array<int32_t, kFc0LayerOutputs> fc0_bias{};
    std::array<int8_t, std::size_t(kFc0LayerOutputs) * kFeatureDims /*placeholder, unused*/> dummy{};
};

struct BucketData {
    std::array<int32_t, kFc0LayerOutputs> fc0_bias{};
    std::array<int8_t, std::size_t(kFc0LayerOutputs) * kTransformedDims> fc0_weight{};

    std::array<int32_t, kL3> fc1_bias{};
    std::array<int8_t, std::size_t(kL3) * kFc1PaddedInputs> fc1_weight{};

    std::array<int32_t, 1> fc2_bias{};
    std::array<int8_t, kFc2Inputs> fc2_weight{};
};

struct SmallNetwork {
    bool is_loaded = false;
    std::string loaded_path;
    std::string desc;
    std::string error;

    std::array<int16_t, kTransformedDims> ft_bias{};
    std::vector<int16_t> ft_weight; // [feature][dim]
    std::vector<int32_t> ft_psqt;   // [feature][bucket]
    std::array<BucketData, kBuckets> buckets{};

    SmallNetwork()
        : ft_weight(std::size_t(kFeatureDims) * kTransformedDims),
          ft_psqt(std::size_t(kFeatureDims) * kBuckets) {}
};

SmallNetwork g_net;

[[nodiscard]] inline int clampi(int x, int lo, int hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

[[nodiscard]] inline Color opposite(Color c) noexcept {
    return c == WHITE ? BLACK : WHITE;
}

template<typename T>
[[nodiscard]] T read_le(std::istream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return value;
}

template<typename T>
bool read_exact(std::istream& in, T* data, std::size_t count) {
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(sizeof(T) * count));
    return !!in;
}

template<std::size_t BlockSize, typename T, std::size_t OrderSize>
void permute_blocks(T* data, std::size_t n, const std::array<std::size_t, OrderSize>& order) {
    const std::size_t total_size = n * sizeof(T);
    const std::size_t process_chunk = BlockSize * OrderSize;
    if (total_size % process_chunk != 0)
        return;

    std::array<std::byte, 16 * 8> buffer{};
    auto* bytes = reinterpret_cast<std::byte*>(data);
    for (std::size_t off = 0; off < total_size; off += process_chunk) {
        std::byte* values = bytes + off;
        for (std::size_t j = 0; j < OrderSize; ++j) {
            std::copy(values + order[j] * BlockSize,
                      values + order[j] * BlockSize + BlockSize,
                      buffer.data() + j * BlockSize);
        }
        std::copy(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(process_chunk), values);
    }
}

constexpr std::array<std::size_t, 8> packus_order() noexcept {
#if defined(__AVX512F__)
    return {0, 2, 4, 6, 1, 3, 5, 7};
#elif defined(__AVX2__)
    return {0, 2, 1, 3, 4, 6, 5, 7};
#else
    return {0, 1, 2, 3, 4, 5, 6, 7};
#endif
}

template<typename T>
bool read_leb128_array(std::istream& in, T* out, std::size_t count) {
    char magic[kLebMagicSize];
    in.read(magic, static_cast<std::streamsize>(kLebMagicSize));
    if (!in) return false;
    if (std::memcmp(magic, kLebMagic, kLebMagicSize) != 0)
        return false;

    uint32_t bytes_left = read_le<uint32_t>(in);
    if (!in) return false;

    std::array<uint8_t, 8192> buf{};
    uint32_t buf_pos = static_cast<uint32_t>(buf.size());

    for (std::size_t i = 0; i < count; ++i) {
        int32_t result = 0;
        std::size_t shift = 0;
        while (true) {
            if (buf_pos == buf.size()) {
                const std::size_t want = std::min<std::size_t>(bytes_left, buf.size());
                in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
                if (!in) return false;
                buf_pos = 0;
            }
            uint8_t byte = buf[buf_pos++];
            --bytes_left;
            result |= static_cast<int32_t>(byte & 0x7f) << (shift % 32);
            shift += 7;
            if ((byte & 0x80) == 0) {
                out[i] = static_cast<T>((shift >= 32 || (byte & 0x40) == 0)
                                            ? result
                                            : (result | ~((1u << shift) - 1)));
                break;
            }
        }
    }

    if (bytes_left != 0) {
        // keep the stream aligned exactly like Stockfish would expect
        in.ignore(bytes_left);
    }
    return !!in;
}

bool read_header(std::istream& in, uint32_t expected_hash, std::string& desc, std::string& error) {
    const uint32_t version = read_le<uint32_t>(in);
    const uint32_t hash = read_le<uint32_t>(in);
    const uint32_t size = read_le<uint32_t>(in);
    if (!in) {
        error = "failed to read NNUE header";
        return false;
    }
    if (version != kVersion) {
        error = "unexpected NNUE version";
        return false;
    }
    if (hash != expected_hash) {
        std::ostringstream oss;
        oss << "network hash mismatch (got 0x" << std::hex << hash
            << ", expected 0x" << expected_hash << ")";
        error = oss.str();
        return false;
    }
    desc.resize(size);
    if (size)
        in.read(desc.data(), static_cast<std::streamsize>(size));
    return !!in;
}

[[nodiscard]] inline int make_halfka_index(Color perspective, Square sq, Piece pc, Square ksq) noexcept {
    const int flip = (perspective == BLACK) ? 56 : 0;
    const int orient = (file_of(ksq) < 4) ? 7 : 0;
    return (sq ^ orient ^ flip) + piece_channel(perspective, pc) + king_buckets[ksq ^ flip];
}

void build_accumulator(const Position& pos, Color perspective, int bucket, int32_t* acc, int32_t& psqt) {
    const Square ksq = king_square(pos, perspective);
    for (int i = 0; i < kTransformedDims; ++i)
        acc[i] = g_net.ft_bias[i];
    psqt = 0;

    Bitboard bb = pieces(pos);
    while (bb) {
        const Square sq = static_cast<Square>(std::countr_zero(static_cast<uint64_t>(bb)));
        bb &= bb - 1;
        const Piece pc = piece_on(pos, sq);
        const int idx = make_halfka_index(perspective, sq, pc, ksq);
        const std::size_t woff = std::size_t(idx) * kTransformedDims;
        for (int i = 0; i < kTransformedDims; ++i)
            acc[i] += g_net.ft_weight[woff + i];
        psqt += g_net.ft_psqt[std::size_t(idx) * kBuckets + bucket];
    }
}

void transform_features(const Position& pos, int bucket, uint8_t* out, int32_t& psqt_out) {
    alignas(64) int32_t acc[COLOR_NB][kTransformedDims];
    int32_t psqt[COLOR_NB] = {0, 0};

    build_accumulator(pos, WHITE, bucket, acc[WHITE], psqt[WHITE]);
    build_accumulator(pos, BLACK, bucket, acc[BLACK], psqt[BLACK]);

    const Color stm = static_cast<Color>(pos.side_to_move);
    const Color nstm = opposite(stm);
    psqt_out = (psqt[stm] - psqt[nstm]) / 2;

    const Color persp[2] = {stm, nstm};
    for (int p = 0; p < 2; ++p) {
        const int offset = p * (kTransformedDims / 2);
        for (int j = 0; j < kTransformedDims / 2; ++j) {
            const int a = clampi(acc[persp[p]][j], 0, 254);
            const int b = clampi(acc[persp[p]][j + kTransformedDims / 2], 0, 254);
            out[offset + j] = static_cast<uint8_t>((static_cast<long long>(a) * b) >> (2 * kWeightScaleBits + 7));
        }
    }
}

int propagate_bucket(const BucketData& net, const uint8_t* tf) noexcept {
    int32_t fc0[kFc0LayerOutputs];
    for (int o = 0; o < kFc0LayerOutputs; ++o) {
        int32_t sum = net.fc0_bias[o];
        const auto* w = &net.fc0_weight[std::size_t(o) * kTransformedDims];
        for (int i = 0; i < kTransformedDims; ++i)
            sum += static_cast<int32_t>(w[i]) * static_cast<int32_t>(tf[i]);
        fc0[o] = sum;
    }

    uint8_t l1[kFc1PaddedInputs]{};
    for (int i = 0; i < kL2; ++i) {
        const int64_t x = fc0[i];
        l1[i] = static_cast<uint8_t>(std::min<int64_t>(127, (x * x) >> (2 * kWeightScaleBits + 7)));
        l1[kL2 + i] = static_cast<uint8_t>(clampi(fc0[i] >> kWeightScaleBits, 0, 127));
    }

    int32_t fc1[kL3];
    for (int o = 0; o < kL3; ++o) {
        int32_t sum = net.fc1_bias[o];
        const auto* w = &net.fc1_weight[std::size_t(o) * kFc1PaddedInputs];
        for (int i = 0; i < kFc1PaddedInputs; ++i)
            sum += static_cast<int32_t>(w[i]) * static_cast<int32_t>(l1[i]);
        fc1[o] = sum;
    }

    uint8_t l2[kFc2Inputs];
    for (int i = 0; i < kFc2Inputs; ++i)
        l2[i] = static_cast<uint8_t>(clampi(fc1[i] >> kWeightScaleBits, 0, 127));

    int32_t out = net.fc2_bias[0];
    for (int i = 0; i < kFc2Inputs; ++i)
        out += static_cast<int32_t>(net.fc2_weight[i]) * static_cast<int32_t>(l2[i]);

    const int32_t fwd = fc0[kL2] * (600 * kOutputScale) / (127 * (1 << kWeightScaleBits));
    return out + fwd;
}

bool load_transformer(std::istream& in, std::string& error) {
    const uint32_t section = read_le<uint32_t>(in);
    if (!in || section != transformer_hash()) {
        std::ostringstream oss;
        oss << "feature transformer hash mismatch (got 0x" << std::hex << section
            << ", expected 0x" << transformer_hash() << ")";
        error = oss.str();
        return false;
    }

    if (!read_leb128_array(in, g_net.ft_bias.data(), g_net.ft_bias.size()) ||
        !read_leb128_array(in, g_net.ft_weight.data(), g_net.ft_weight.size()) ||
        !read_leb128_array(in, g_net.ft_psqt.data(), g_net.ft_psqt.size())) {
        error = "failed to read feature transformer parameters";
        return false;
    }

    const auto order = packus_order();
    permute_blocks<16>(g_net.ft_bias.data(), g_net.ft_bias.size(), order);
    permute_blocks<16>(g_net.ft_weight.data(), g_net.ft_weight.size(), order);
    for (auto& v : g_net.ft_bias) v = static_cast<int16_t>(v * 2);
    for (auto& v : g_net.ft_weight) v = static_cast<int16_t>(v * 2);

    return true;
}

bool load_bucket(std::istream& in, BucketData& b, std::string& error) {
    const uint32_t section = read_le<uint32_t>(in);
    if (!in || section != bucket_arch_hash()) {
        std::ostringstream oss;
        oss << "bucket architecture hash mismatch (got 0x" << std::hex << section
            << ", expected 0x" << bucket_arch_hash() << ")";
        error = oss.str();
        return false;
    }

    if (!read_exact(in, b.fc0_bias.data(), b.fc0_bias.size()) ||
        !read_exact(in, b.fc0_weight.data(), b.fc0_weight.size()) ||
        !read_exact(in, b.fc1_bias.data(), b.fc1_bias.size()) ||
        !read_exact(in, b.fc1_weight.data(), b.fc1_weight.size()) ||
        !read_exact(in, b.fc2_bias.data(), b.fc2_bias.size()) ||
        !read_exact(in, b.fc2_weight.data(), b.fc2_weight.size())) {
        error = "failed to read bucket parameters";
        return false;
    }
    return true;
}

} // namespace

bool load(const std::string& path) {
    unload();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        g_net.error = "cannot open NNUE file: " + path;
        return false;
    }

    if (!read_header(in, network_hash(), g_net.desc, g_net.error))
        return false;
    if (!load_transformer(in, g_net.error))
        return false;
    for (auto& bucket : g_net.buckets)
        if (!load_bucket(in, bucket, g_net.error))
            return false;

    if (in.peek() != std::istream::traits_type::eof()) {
        g_net.error = "unexpected trailing data in NNUE file";
        unload();
        return false;
    }

    g_net.is_loaded = true;
    g_net.loaded_path = path;
    return true;
}

void unload() noexcept {
    g_net.is_loaded = false;
    g_net.loaded_path.clear();
    g_net.desc.clear();
    g_net.error.clear();
    std::fill(g_net.ft_bias.begin(), g_net.ft_bias.end(), int16_t{0});
    std::fill(g_net.ft_weight.begin(), g_net.ft_weight.end(), int16_t{0});
    std::fill(g_net.ft_psqt.begin(), g_net.ft_psqt.end(), int32_t{0});
    for (auto& b : g_net.buckets) {
        std::fill(b.fc0_bias.begin(), b.fc0_bias.end(), int32_t{0});
        std::fill(b.fc0_weight.begin(), b.fc0_weight.end(), int8_t{0});
        std::fill(b.fc1_bias.begin(), b.fc1_bias.end(), int32_t{0});
        std::fill(b.fc1_weight.begin(), b.fc1_weight.end(), int8_t{0});
        std::fill(b.fc2_bias.begin(), b.fc2_bias.end(), int32_t{0});
        std::fill(b.fc2_weight.begin(), b.fc2_weight.end(), int8_t{0});
    }
}

bool loaded() noexcept { return g_net.is_loaded; }
const std::string& path() noexcept { return g_net.loaded_path; }
const std::string& description() noexcept { return g_net.desc; }
const std::string& last_error() noexcept { return g_net.error; }

int eval(const Position& pos) noexcept {
    if (!g_net.is_loaded)
        return 0;

    const int piece_count = std::popcount(static_cast<uint64_t>(pieces(pos)));
    const int bucket = clampi((piece_count - 1) / 4, 0, kBuckets - 1);

    alignas(64) uint8_t transformed[kTransformedDims]{};
    int32_t psqt = 0;
    transform_features(pos, bucket, transformed, psqt);
    const int32_t positional = propagate_bucket(g_net.buckets[bucket], transformed);
    return static_cast<int>((psqt + positional) / kOutputScale);
}

WinRateParams win_rate_params(const Position& pos) noexcept {
    const int material =
        std::popcount(pieces(pos, WHITE, PAWN)   | pieces(pos, BLACK, PAWN)) +
    3 * std::popcount(pieces(pos, WHITE, KNIGHT) | pieces(pos, BLACK, KNIGHT)) +
    3 * std::popcount(pieces(pos, WHITE, BISHOP) | pieces(pos, BLACK, BISHOP)) +
    5 * std::popcount(pieces(pos, WHITE, ROOK)   | pieces(pos, BLACK, ROOK)) +
    9 * std::popcount(pieces(pos, WHITE, QUEEN)  | pieces(pos, BLACK, QUEEN));

    const double m = std::clamp(material, 17, 78) / 58.0;

    constexpr double as[] = {-72.32565836, 185.93832038, -144.58862193, 416.44950446};
    constexpr double bs[] = {83.86794042, -136.06112997, 69.98820887, 47.62901433};

    const double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    const double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}

int to_cp(int v, const Position& pos) noexcept {
    auto [a, b] = win_rate_params(pos);
    (void)b;
    return static_cast<int>(std::round(100.0 * static_cast<double>(v) / a));
}

int win_rate_model(int v, const Position& pos) noexcept {
    auto [a, b] = win_rate_params(pos);
    return static_cast<int>(0.5 + 1000.0 / (1.0 + std::exp((a - static_cast<double>(v)) / b)));
}

} // namespace valerain::nnue
