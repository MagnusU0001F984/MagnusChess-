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

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include "Memory.h"
#include "Position.h"

namespace valerain {

using NodeCount = std::uint64_t;

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

GenSpeedResult benchmark_generation(
    const Position& pos,
    const memory::Memory& mem,
    std::uint64_t iterations
);

} // namespace valerain
