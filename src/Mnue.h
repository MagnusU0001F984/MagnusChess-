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

#pragma once

#include <ostream>
#include <string>

#include "Nnue.h"
#include "Position.h"

namespace magnus::mnue {

using WinRateParams = nnue::WinRateParams;
using WdlTriplet = nnue::WdlTriplet;

// MNUE-P2: fast filter net.
//   1 x 32 x 16 x 1024 x 10240
//   output_buckets x input_buckets x hidden x input_features.
struct P2Layout {
    static constexpr int InputBuckets = 16;
    static constexpr int OutputBuckets = 32;
    static constexpr int RelativeColors = 2;
    static constexpr int NonKingPieceTypes = 5;
    static constexpr int Squares = 64;
    static constexpr int InputSize = InputBuckets * RelativeColors * NonKingPieceTypes * Squares;
    static constexpr int HiddenSize = 1024;
    static constexpr int ArchId = 2;
};

// MNUE-P4: precision refine net.
//   1 x 32 x 32 x 5120 x 20480
//   output_buckets x input_buckets x hidden x input_features.
struct P4Layout {
    static constexpr int InputBuckets = 32;
    static constexpr int OutputBuckets = 32;
    static constexpr int RelativeColors = 2;
    static constexpr int NonKingPieceTypes = 5;
    static constexpr int Squares = 64;
    static constexpr int InputSize = InputBuckets * RelativeColors * NonKingPieceTypes * Squares;
    static constexpr int HiddenSize = 5120;
    static constexpr int ArchId = 4;
};

static_assert(P2Layout::InputSize == 10240);
static_assert(P4Layout::InputSize == 20480);

bool load_p2(const std::string& path);
bool load_p4(const std::string& path);
void unload_p2() noexcept;
void unload_p4() noexcept;
void unload_all() noexcept;

[[nodiscard]] bool p2_loaded() noexcept;
[[nodiscard]] bool p4_loaded() noexcept;
[[nodiscard]] const std::string& p2_path() noexcept;
[[nodiscard]] const std::string& p4_path() noexcept;
[[nodiscard]] const std::string& last_error() noexcept;

// P2 is the base evaluator. P4 is intentionally lazy and should only be used
// from selected search nodes; it is not written into TT raw eval in the first
// integration stage.
[[nodiscard]] int eval_p2(const Position& pos) noexcept;
[[nodiscard]] int eval_p4_lazy(const Position& pos) noexcept;
[[nodiscard]] int debug_eval_p2_reference(const Position& pos) noexcept;
[[nodiscard]] bool p2_i32_forward_enabled() noexcept;
[[nodiscard]] int p2_w1_max_abs() noexcept;


// Debug/check helper: compares persistent P2 incremental accumulator against
// a fresh full rebuild for the current position.
[[nodiscard]] bool debug_check_p2_incremental(
    const Position& pos,
    std::ostream& out
) noexcept;


// Incremental P2 accumulator hooks. These are called from Position mutators.
// P2 keeps a persistent accumulator; P4 intentionally remains lazy-rebuild.
void on_position_cleared(Position& pos) noexcept;
void on_piece_added(Position& pos, Color color, PieceType piece_type, Square sq) noexcept;
void on_piece_removed(Position& pos, Color color, PieceType piece_type, Square sq) noexcept;
void on_piece_moved(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept;

// Debug helper for verifying trainer/engine feature-index alignment.
// UCI command `mnuedebug` calls this and prints P2 input buckets,
// active feature indices, output bucket, and raw P2 eval.
void debug_dump_p2_features(const Position& pos, std::ostream& out);

[[nodiscard]] int search_score(int v, const Position& pos) noexcept;
[[nodiscard]] int material_units(const Position& pos) noexcept;
[[nodiscard]] WinRateParams win_rate_params(const Position& pos) noexcept;
[[nodiscard]] int to_cp(int v, const Position& pos) noexcept;
[[nodiscard]] int win_rate_model(int v, const Position& pos) noexcept;
[[nodiscard]] int search_score_to_cp(int score, const Position& pos) noexcept;
[[nodiscard]] int search_score_to_winrate(int score, const Position& pos) noexcept;
[[nodiscard]] WdlTriplet search_score_to_wdl(int score, const Position& pos) noexcept;

} // namespace magnus::mnue
