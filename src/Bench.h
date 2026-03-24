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

#include "Memory.h"
#include "Perft.h"
#include "Position.h"

namespace valerain {

struct BenchConfig {
    int perft_depth = 10;
    std::size_t hash_mb = 64ULL;
    std::size_t threads = 1ULL;
    bool divide = false;
    bool live_divide = false;
};

struct PerftBenchResult {
    int depth = 0;
    NodeCount nodes = 0;
    double seconds = 0.0;
    double nps = 0.0;
    std::size_t threads = 1ULL;
};
void set_start_position(Position& pos) noexcept;

[[nodiscard]] PerftBenchResult benchmark_perft(
    const Position& pos,
    const memory::Memory& mem,
    int depth,
    std::size_t threads
);

[[nodiscard]] BenchConfig parse_config(int argc, char** argv) noexcept;
int run_bench(int argc, char** argv);

} // namespace valerain
