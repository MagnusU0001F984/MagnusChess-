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

#include "Attack.h"
#include "Perft.h"
#include "MoveGen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace valerain {
namespace {

constexpr const char* kAnsiRed = "\x1b[31m";
constexpr const char* kAnsiGreen = "\x1b[32m";
constexpr const char* kAnsiReset = "\x1b[0m";

NodeCount perft_serial(const Position& pos, const memory::Memory& mem, int depth) {
    if (depth <= 0) return 1;

    MoveList list{};
    generate_legal(pos, mem, list);

    if (depth == 1)
        return static_cast<NodeCount>(list.size);

    NodeCount nodes = 0;

    for (int i = 0; i < list.size; ++i) {
        Position next = pos;
        do_move_copy(next, list.moves[i]);
        nodes += perft_serial(next, mem, depth - 1);
    }

    return nodes;
}

struct PerftTask {
    Position pos;
    int depth;
};

struct PerftDivideResult {
    std::vector<PerftDivideRow> rows;
    NodeCount total_nodes = 0;
    double total_seconds = 0.0;
    int root_moves = 0;
};

[[nodiscard]] char promo_char(PieceType pt) noexcept {
    switch (pt) {
        case KNIGHT: return 'n';
        case BISHOP: return 'b';
        case ROOK:   return 'r';
        case QUEEN:  return 'q';
        default:     return '\0';
    }
}

[[nodiscard]] std::string move_to_uci(Move m) {
    std::string s;
    s.reserve(5);

    const Square from = from_sq(m);
    const Square to = to_sq(m);

    s.push_back(static_cast<char>('a' + file_of(from)));
    s.push_back(static_cast<char>('1' + rank_of(from)));
    s.push_back(static_cast<char>('a' + file_of(to)));
    s.push_back(static_cast<char>('1' + rank_of(to)));

    if (move_is_promotion(m))
        s.push_back(promo_char(promo_piece(m)));

    return s;
}

[[nodiscard]] std::string format_u64_with_commas(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<std::size_t>(i), ",");
    return digits;
}

[[nodiscard]] std::string format_fixed_with_commas(double value, int decimals) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    std::string s = oss.str();

    std::size_t dot = s.find('.');
    if (dot == std::string::npos)
        dot = s.size();

    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(dot) - 3; i > 0; i -= 3)
        s.insert(static_cast<std::size_t>(i), ",");

    return s;
}

[[nodiscard]] std::string format_seconds(double seconds) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << seconds << " s";
    return oss.str();
}

[[nodiscard]] std::string format_knps(double knps) {
    return format_fixed_with_commas(knps, 1) + " kN/s";
}

[[nodiscard]] std::string divide_separator() {
    return std::string(60, '-');
}

[[nodiscard]] std::string colored_marker(bool highlight) {
    return std::string(highlight ? kAnsiRed : kAnsiGreen)
        + (highlight ? "[*]" : "[-]")
        + kAnsiReset;
}

void render_divide_header(std::ostream& os, int depth, int root_moves, std::size_t threads) {
    os << divide_separator() << "\n";
    os << " " << colored_marker(true) << " perft " << depth
       << "   backend: " << attack_backend_name()
       << "   root moves: " << root_moves
       << "   threads: " << threads << "\n";
    os << divide_separator() << "\n";
}

void render_divide_table_header(std::ostream& os) {
    std::ostringstream row;
    row << " " << colored_marker(true) << " "
        << std::setw(3) << std::right << "#"
        << "   "
        << std::setw(8) << std::left << "Move"
        << std::setw(12) << std::right << "Nodes"
        << "  "
        << std::setw(10) << std::right << "Time"
        << "  "
        << std::setw(18) << std::right << "NPS";
    os << row.str() << "\n";
    os << divide_separator() << "\n";
}

void render_divide_row(std::ostream& os, const PerftDivideRow& row) {
    std::ostringstream line;
    line << " " << colored_marker(false) << " "
         << std::setw(3) << std::right << row.index
         << "   "
         << std::setw(8) << std::left << row.move
         << std::setw(12) << std::right << format_u64_with_commas(row.nodes)
         << "  "
         << std::setw(10) << std::right << format_seconds(row.seconds)
         << "  "
         << std::setw(18) << std::right << format_knps(row.knps);
    os << line.str() << "\n";
}

void render_divide_total(std::ostream& os, NodeCount nodes, double seconds) {
    const double knps = seconds > 0.0 ? (static_cast<double>(nodes) / seconds) / 1000.0 : 0.0;

    os << divide_separator() << "\n";

    std::ostringstream line;
    line << " " << colored_marker(true) << " "
         << std::setw(3) << std::right << ""
         << "   "
         << std::setw(8) << std::left << "Total"
         << std::setw(12) << std::right << format_u64_with_commas(nodes)
         << "  "
         << std::setw(10) << std::right << format_seconds(seconds)
         << "  "
         << std::setw(18) << std::right << format_knps(knps);
    os << line.str() << "\n";
    os << divide_separator() << "\n";
}

PerftDivideResult compute_divide_result(const Position& pos,
                                        const memory::Memory& mem,
                                        int depth,
                                        std::size_t threads,
                                        bool live,
                                        std::ostream& os) {
    using clock = std::chrono::steady_clock;

    MoveList list{};
    generate_legal(pos, mem, list);

    PerftDivideResult result;
    result.root_moves = list.size;
    result.rows.reserve(static_cast<std::size_t>(list.size));

    const auto total_start = clock::now();

    if (live) {
        render_divide_header(os, depth, list.size, threads);
        render_divide_table_header(os);
    }

    for (int i = 0; i < list.size; ++i) {
        const Move m = list.moves[i];
        Position next = pos;
        do_move_copy(next, m);

        const auto start = clock::now();
        const NodeCount child = (depth <= 1)
            ? 1
            : (threads > 1 ? perft_mt(next, mem, depth - 1, threads)
                           : perft(next, mem, depth - 1));
        const auto end = clock::now();

        const double seconds = std::chrono::duration<double>(end - start).count();
        const double knps = seconds > 0.0 ? (static_cast<double>(child) / seconds) / 1000.0 : 0.0;

        PerftDivideRow row;
        row.index = i + 1;
        row.move = move_to_uci(m);
        row.nodes = child;
        row.seconds = seconds;
        row.knps = knps;

        result.total_nodes += child;

        if (live)
            render_divide_row(os, row);
        else
            result.rows.push_back(row);
    }

    result.total_seconds = std::chrono::duration<double>(clock::now() - total_start).count();
    return result;
}

void expand_frontier_once(
    const std::vector<PerftTask>& in,
    const memory::Memory& mem,
    std::vector<PerftTask>& out,
    NodeCount& finished_nodes
) {
    out.clear();

    for (const PerftTask& task : in) {
        if (task.depth <= 0) {
            finished_nodes += 1;
            continue;
        }

        MoveList list{};
        generate_legal(task.pos, mem, list);

        if (task.depth == 1) {
            finished_nodes += static_cast<NodeCount>(list.size);
            continue;
        }

        for (int i = 0; i < list.size; ++i) {
            Position next = task.pos;
            do_move_copy(next, list.moves[i]);
            out.push_back(PerftTask{next, task.depth - 1});
        }
    }
}

} // namespace

NodeCount perft(const Position& pos, const memory::Memory& mem, int depth) {
    return perft_serial(pos, mem, depth);
}

NodeCount perft_mt(
    const Position& pos,
    const memory::Memory& mem,
    int depth,
    std::size_t threads
) {
    if (depth <= 0)
        return 1;

    if (threads <= 1 || depth <= 2)
        return perft_serial(pos, mem, depth);

    // 关键改动：
    // 不再只按 root moves 分任务，而是先浅层展开出足够多的 frontier tasks。
    constexpr int max_split_ply = 2;
    const std::size_t target_tasks = std::max<std::size_t>(threads * 16, 128);

    NodeCount finished_nodes = 0;
    std::vector<PerftTask> frontier;
    std::vector<PerftTask> next_frontier;
    frontier.push_back(PerftTask{pos, depth});

    int split_ply = 0;
    while (split_ply < max_split_ply && frontier.size() < target_tasks) {
        expand_frontier_once(frontier, mem, next_frontier, finished_nodes);
        frontier.swap(next_frontier);
        ++split_ply;

        if (frontier.empty())
            break;
    }

    if (frontier.empty())
        return finished_nodes;

    const std::size_t worker_count =
        std::min<std::size_t>(threads, frontier.size());

    std::atomic<std::size_t> next_index{0};
    std::vector<NodeCount> partial(worker_count, 0);
    std::vector<std::thread> pool;
    pool.reserve(worker_count);

    for (std::size_t tid = 0; tid < worker_count; ++tid) {
        pool.emplace_back([&, tid]() {
            NodeCount local = 0;

            for (;;) {
                const std::size_t i =
                    next_index.fetch_add(1, std::memory_order_relaxed);
                if (i >= frontier.size())
                    break;

                local += perft_serial(frontier[i].pos, mem, frontier[i].depth);
            }

            partial[tid] = local;
        });
    }

    for (std::thread& t : pool)
        t.join();

    NodeCount nodes = finished_nodes;
    for (NodeCount x : partial)
        nodes += x;

    return nodes;
}

void divide(const Position& pos,
            const memory::Memory& mem,
            int depth,
            std::ostream& os,
            std::size_t threads,
            bool live) {
    const PerftDivideResult result = compute_divide_result(pos, mem, depth, threads, live, os);

    if (!live) {
        render_divide_header(os, depth, result.root_moves, threads);
        render_divide_table_header(os);
        for (const PerftDivideRow& row : result.rows)
            render_divide_row(os, row);
    }

    render_divide_total(os, result.total_nodes, result.total_seconds);
}

GenSpeedResult benchmark_generation(
    const Position& pos,
    const memory::Memory& mem,
    std::uint64_t iterations
) {
    using clock = std::chrono::steady_clock;

    std::uint64_t total_moves = 0;
    auto start = clock::now();

    for (std::uint64_t i = 0; i < iterations; ++i) {
        MoveList list{};
        generate_legal(pos, mem, list);
        total_moves += static_cast<std::uint64_t>(list.size);
    }

    auto end = clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();

    GenSpeedResult r;
    r.iterations = iterations;
    r.total_moves = total_moves;
    r.seconds = seconds;
    r.generations_per_second = seconds > 0.0 ? iterations / seconds : 0.0;
    r.moves_per_second = seconds > 0.0 ? total_moves / seconds : 0.0;
    return r;
}

} // namespace valerain
