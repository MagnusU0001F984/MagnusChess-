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

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <string>

#include "Memory.h"
#include "Position.h"
#include "Types.h"

namespace valerain::search {

/*
This header exposes the search entry points used by both the bench command and
the UCI loop. The implementation is a classical iterative-deepening PVS search
with a small set of pragmatic pruning heuristics.
*/

constexpr int MAX_PLY = 128;

struct SearchLimits {
    // Depth, node, and time limits supplied by bench mode or UCI.
    int depth = MAX_PLY;
    u64 node_limit = 0;
    int soft_time_ms = 0;
    int hard_time_ms = 0;
    bool infinite = false;
    bool use_nnue = false;
    Move root_moves[256]{};
    int root_move_count = 0;
    const std::atomic<bool>* stop = nullptr;
};

struct SearchResult {
    // Final root move plus the score and aggregate search statistics.
    Move best_move = 0;
    Score score = 0;
    u64 nodes = 0;
    int depth = 0;
    int seldepth = 0;
};

// Converts the internal 16-bit move format into UCI coordinate notation.
[[nodiscard]] std::string move_to_uci(Move m);

[[nodiscard]] SearchResult iterative_deepening(
    const Position& root,
    memory::Memory& mem,
    const SearchLimits& limits,
    std::ostream* out = nullptr
);

} // namespace valerain::search
