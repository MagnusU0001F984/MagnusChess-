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
#include <string>

namespace valerain::nnue {

namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u32 = std::uint32_t;

constexpr u32 kMagic   = 0x554E4E56u; // "VNNU" in little-endian file bytes
constexpr u32 kVersion = 1;

constexpr int kInputs = 768;   // 2 colors * 6 piece types * 64 squares
constexpr int kHidden = 128;
constexpr int kClip   = 255;
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
    std::string loaded_path;
    std::string desc;
    std::string error;

    i32 scale = kDefaultScale;

    // First layer: [input][hidden]
    std::array<i16, kInputs * kHidden> w0{};
    std::array<i16, kHidden> b0{};

    // Output layer over dual-perspective hidden activations: [us_hidden | them_hidden]
    std::array<i16, 2 * kHidden> w1{};
    i32 b1 = 0;
};

NativeNetwork g_net{};

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

void clear_network() noexcept {
    g_net.is_loaded = false;
    g_net.loaded_path.clear();
    g_net.desc.clear();
    g_net.error.clear();
    g_net.scale = kDefaultScale;
    g_net.w0.fill(0);
    g_net.b0.fill(0);
    g_net.w1.fill(0);
    g_net.b1 = 0;
}

void refresh_hidden(
    const Position& pos,
    Color persp,
    i32* acc
) noexcept {
    for (int i = 0; i < kHidden; ++i)
        acc[i] = g_net.b0[i];

    Bitboard bb = pieces(pos);
    while (bb) {
        const Square sq =
            static_cast<Square>(std::countr_zero(static_cast<std::uint64_t>(bb)));
        bb &= (bb - 1);

        const Piece pc = piece_on(pos, sq);
        const int idx = chess768_index(persp, pc, sq);
        if (idx < 0)
            continue;

        const i16* w = &g_net.w0[static_cast<std::size_t>(idx) * kHidden];
        for (int i = 0; i < kHidden; ++i)
            acc[i] += w[i];
    }
}

[[nodiscard]] int forward(const Position& pos) noexcept {
    alignas(64) i32 us[kHidden];
    alignas(64) i32 them[kHidden];

    const Color stm = static_cast<Color>(pos.side_to_move);
    const Color nstm = opposite(stm);

    refresh_hidden(pos, stm, us);
    refresh_hidden(pos, nstm, them);

    i32 sum = g_net.b1;
    for (int i = 0; i < kHidden; ++i) {
        const i32 a = std::clamp(us[i],   0, kClip);
        const i32 b = std::clamp(them[i], 0, kClip);

        sum += static_cast<i32>(g_net.w1[i]) * a;
        sum += static_cast<i32>(g_net.w1[kHidden + i]) * b;
    }

    return static_cast<int>(sum / std::max<i32>(1, g_net.scale));
}

} // namespace

bool load(const std::string& path) {
    unload();

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

    if (in.peek() != std::istream::traits_type::eof()) {
        clear_network();
        g_net.error = "unexpected trailing data in NNUE file";
        return false;
    }

    g_net.is_loaded = true;
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

    if (std::abs(a) < 1e-9)
        return v;

    return static_cast<int>(std::round(100.0 * static_cast<double>(v) / a));
}

int win_rate_model(int v, const Position& pos) noexcept {
    auto [a, b] = win_rate_params(pos);

    if (std::abs(b) < 1e-9)
        return v >= static_cast<int>(a) ? 1000 : 0;

    return static_cast<int>(
        0.5 + 1000.0 / (1.0 + std::exp((a - static_cast<double>(v)) / b))
    );
}

} // namespace valerain::nnue