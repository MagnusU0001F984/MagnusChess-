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

#include <cstddef>
#include <cstdint>
#include <iosfwd>

#include "Memory.h"
#include "Perft.h"
#include "Position.h"

namespace magnus {

/*
Bench mode is the command-line utility layer used for quick perft checks and
fixed-depth search/bench benchmarks outside of UCI.
*/

inline constexpr int DEFAULT_BENCH_MOVETIME_MS = 1000;

/*
 * Bench 模組公開介面
 * BenchConfig — 命令列參數（perft_depth/search_depth/movetime/threads）
 * PerftBenchResult — 基準測試計時結果
 * set_start_position() — 建立標準西洋棋初始局面
 */
struct BenchConfig {
    int perft_depth = 10;
    int search_depth = 6;
    int search_movetime_ms = DEFAULT_BENCH_MOVETIME_MS;
    int eval_iterations = 20000;
    std::size_t hash_mb = 16ULL;
    std::size_t threads = 1ULL;
    bool divide = false;
    bool evalbench = false;
    bool search = false;
    bool timed_search = false;
    bool live_divide = false;
};

struct PerftBenchResult {
    int depth = 0;
    NodeCount nodes = 0;
    double seconds = 0.0;
    double nps = 0.0;
    std::size_t threads = 1ULL;
};

// Creates the classical initial chess position used by perft and bench mode.
void set_start_position(Position& pos) noexcept;

[[nodiscard]] PerftBenchResult benchmark_perft(
    const Position& pos,
    const memory::Memory& mem,
    int depth,
    std::size_t threads
);

[[nodiscard]] bool run_search_bench(
    memory::Memory& mem,
    int depth,
    std::size_t threads,
    bool use_nnue,
    bool emit_ponder,
    std::ostream& out
);

[[nodiscard]] bool run_timed_search_bench(
    memory::Memory& mem,
    int movetime_ms,
    std::size_t threads,
    bool use_nnue,
    bool emit_ponder,
    std::ostream& out
);

[[nodiscard]] BenchConfig parse_config(int argc, char** argv) noexcept;
int run_bench(int argc, char** argv);

} // namespace magnus
