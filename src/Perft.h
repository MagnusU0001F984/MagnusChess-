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

/* ===== ANNOTATED: 繁體中文註釋已添加 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 詳細說明請參閱對應的 .cpp 實作檔案。
 */


#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include "Memory.h"
#include "Position.h"

namespace magnus {

/*
Perft is used to validate move generation by counting legal move trees exactly.
This file also exposes a small benchmarking surface for move-generator speed.
*/

using NodeCount = std::uint64_t;

/*
 * Perft 模組公開介面
 * PerftDivideRow — 分割輸出的單行（著法/節點數/耗時）
 * GenSpeedResult — 著法生成器速度基準測試結果
 * perft/perft_mt/divide/benchmark_generation — 核心 API
 */
struct PerftDivideRow {
    int index = 0;
    std::string move;
    NodeCount nodes = 0;
    double seconds = 0.0;
    double knps = 0.0;
};

NodeCount perft(const Position& pos, const memory::Memory& mem, int depth);
NodeCount perft_mt(
    const Position& pos,
    const memory::Memory& mem,
    int depth,
    std::size_t threads
);

void divide(const Position& pos,
            const memory::Memory& mem,
            int depth,
            std::ostream& os,
            std::size_t threads = 1,
            bool live = false);

struct GenSpeedResult {
    std::uint64_t iterations = 0;
    std::uint64_t total_moves = 0;
    double seconds = 0.0;
    double generations_per_second = 0.0;
    double moves_per_second = 0.0;
};

// Repeatedly times legal move generation on a fixed position.
GenSpeedResult benchmark_generation(
    const Position& pos,
    const memory::Memory& mem,
    std::uint64_t iterations
);

} // namespace magnus
