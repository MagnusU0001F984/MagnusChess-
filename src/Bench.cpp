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
#include "Bench.h"
#include "Memory.h"
#include "MoveGen.h"
#include "Perft.h"
#include "Position.h"
#include "Search.h"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace valerain {

/*
Bench mode is deliberately tiny: create a start position, initialize shared
tables, then route either to perft/divide or to the fixed-depth search smoke test.
*/

namespace {

[[nodiscard]] bool list_contains_move(const MoveList& list, Move move) noexcept {
    for (int i = 0; i < list.size; ++i)
        if (list.moves[i] == move)
            return true;
    return false;
}

Position make_pinned_ep_regression_position(const memory::Memory& mem) noexcept {
    Position pos{};
    position_clear(pos);

    pos.side_to_move = WHITE;
    pos.ep_sq = 44; // e6
    pos.castling_rights = NO_CASTLING;
    pos.halfmove_clock = 0;
    pos.fullmove_number = 1;

    position_put_piece(pos, BLACK, KING, 56);   // a8
    position_put_piece(pos, BLACK, BISHOP, 62); // g8
    position_put_piece(pos, BLACK, PAWN, 36);   // e5
    position_put_piece(pos, WHITE, PAWN, 35);   // d5
    position_put_piece(pos, WHITE, KING, 26);   // c4

    position_refresh_key(pos, mem.tables);
    return pos;
}

[[nodiscard]] bool regression_root_stop_keeps_legal_fallback(
    memory::Memory& mem,
    std::ostream& out
) {
    memory::memory_clear_hash(mem);

    Position pos{};
    set_start_position(pos);
    position_refresh_key(pos, mem.tables);

    std::atomic<bool> stop_requested{true};
    search::SearchLimits limits{};
    limits.depth = 1;
    limits.stop = &stop_requested;

    const search::SearchResult result =
        search::iterative_deepening(pos, mem, limits, nullptr);

    Position work = pos;
    const bool ok =
        !move_is_none(result.best_move) &&
        legal(work, mem, result.best_move);

    out << (ok ? "[PASS] " : "[FAIL] ")
        << "root stop fallback move: "
        << search::move_to_uci(result.best_move) << '\n';
    return ok;
}

[[nodiscard]] bool regression_interrupted_search_skips_root_tt(
    memory::Memory& mem,
    std::ostream& out
) {
    memory::memory_clear_hash(mem);

    Position pos{};
    set_start_position(pos);
    position_refresh_key(pos, mem.tables);

    search::SearchLimits limits{};
    limits.depth = 2;
    limits.node_limit = 1;

    (void)search::iterative_deepening(pos, mem, limits, nullptr);

    const memory::TTProbe probe = memory::tt_probe(mem.tt, pos.key);
    const bool ok = !probe.hit;

    out << (ok ? "[PASS] " : "[FAIL] ")
        << "interrupted root TT save: hit=" << (probe.hit ? "true" : "false");
    if (probe.hit) {
        out << " move=" << search::move_to_uci(static_cast<Move>(probe.data.move))
            << " depth=" << probe.data.depth
            << " flags=" << static_cast<int>(probe.data.flags);
    }
    out << '\n';
    return ok;
}

[[nodiscard]] bool regression_pinned_ep_is_generated(
    const memory::Memory& mem,
    std::ostream& out
) {
    Position pos = make_pinned_ep_regression_position(mem);
    MoveList list{};
    generate_legal(pos, mem, list);

    const Move ep_move = make_move(35, 44, MOVE_EP); // d5e6 ep
    const bool ok = list.size == 7 && list_contains_move(list, ep_move);

    out << (ok ? "[PASS] " : "[FAIL] ")
        << "pinned EP generation: size=" << list.size
        << " contains_d5e6=" << (list_contains_move(list, ep_move) ? "true" : "false")
        << '\n';
    return ok;
}

} // namespace

void set_start_position(Position& pos) noexcept {
    // Rebuild the standard initial chess position through the public mutators so
    // all caches and bitboards are initialized exactly as search expects.
    position_clear(pos);

    pos.side_to_move = WHITE;
    pos.ep_sq = NO_SQ;
    pos.castling_rights = ANY_CASTLING;
    pos.halfmove_clock = 0;
    pos.fullmove_number = 1;

    for (int sq = 8; sq < 16; ++sq)
        position_put_piece(pos, WHITE, PAWN, sq);
    for (int sq = 48; sq < 56; ++sq)
        position_put_piece(pos, BLACK, PAWN, sq);

    position_put_piece(pos, WHITE, ROOK, 0);
    position_put_piece(pos, WHITE, KNIGHT, 1);
    position_put_piece(pos, WHITE, BISHOP, 2);
    position_put_piece(pos, WHITE, QUEEN, 3);
    position_put_piece(pos, WHITE, KING, 4);
    position_put_piece(pos, WHITE, BISHOP, 5);
    position_put_piece(pos, WHITE, KNIGHT, 6);
    position_put_piece(pos, WHITE, ROOK, 7);

    position_put_piece(pos, BLACK, ROOK, 56);
    position_put_piece(pos, BLACK, KNIGHT, 57);
    position_put_piece(pos, BLACK, BISHOP, 58);
    position_put_piece(pos, BLACK, QUEEN, 59);
    position_put_piece(pos, BLACK, KING, 60);
    position_put_piece(pos, BLACK, BISHOP, 61);
    position_put_piece(pos, BLACK, KNIGHT, 62);
    position_put_piece(pos, BLACK, ROOK, 63);
}

PerftBenchResult benchmark_perft(
    const Position& pos,
    const memory::Memory& mem,
    int depth,
    std::size_t threads
) {
    using clock = std::chrono::steady_clock;

    const auto start = clock::now();
    const NodeCount nodes = threads > 1
        ? perft_mt(pos, mem, depth, threads)
        : perft(pos, mem, depth);
    const auto end = clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();

    PerftBenchResult r;
    r.depth = depth;
    r.nodes = nodes;
    r.seconds = seconds;
    r.nps = seconds > 0.0 ? static_cast<double>(nodes) / seconds : 0.0;
    r.threads = threads;
    return r;
}

BenchConfig parse_config(int argc, char** argv) noexcept {
    BenchConfig cfg;
    int argi = 1;

    if (argc > 1 && std::string_view(argv[1]) == "divide") {
        cfg.divide = true;
        argi = 2;
    }
    else if (argc > 1 && std::string_view(argv[1]) == "search") {
        cfg.search = true;
        argi = 2;
    }
    else if (argc > 1 &&
             (std::string_view(argv[1]) == "test" ||
              std::string_view(argv[1]) == "regression")) {
        cfg.regression = true;
        argi = 2;
    }

    if (cfg.search) {
        if (argc > argi) cfg.search_depth = std::max(1, std::atoi(argv[argi]));
        if (argc > argi + 1) cfg.hash_mb = static_cast<std::size_t>(std::strtoull(argv[argi + 1], nullptr, 10));
    } else {
        if (argc > argi) cfg.perft_depth = std::max(0, std::atoi(argv[argi]));
        if (argc > argi + 1) cfg.hash_mb = static_cast<std::size_t>(std::strtoull(argv[argi + 1], nullptr, 10));
        if (argc > argi + 2) cfg.threads = static_cast<std::size_t>(std::strtoull(argv[argi + 2], nullptr, 10));
        if (argc > argi + 3 && std::string_view(argv[argi + 3]) == "live")
            cfg.live_divide = true;
    }
    if (cfg.hash_mb == 0) cfg.hash_mb = 1;
    if (cfg.threads == 0) cfg.threads = 1;

    return cfg;
}

int run_regression_tests() {
    memory::Memory mem{};
    memory_init(mem, 16, 8, 2);
    attack_init_backend(mem);

    int failures = 0;
    failures += regression_root_stop_keeps_legal_fallback(mem, std::cout) ? 0 : 1;
    failures += regression_interrupted_search_skips_root_tt(mem, std::cout) ? 0 : 1;
    failures += regression_pinned_ep_is_generated(mem, std::cout) ? 0 : 1;

    memory_free(mem);
    return failures == 0 ? 0 : 1;
}

int run_bench(int argc, char** argv) { 
    const BenchConfig cfg = parse_config(argc, argv);

    if (cfg.regression)
        return run_regression_tests();

    memory::Memory mem{};
    memory_init(mem, cfg.hash_mb, 8, 2);
    attack_init_backend(mem);

    Position pos{};
    set_start_position(pos);

    if (!position_has_valid_kings(pos) || !position_board_matches_bitboards(pos)) {
        std::cerr << "position bootstrap failed\n";
        memory_free(mem);
        return 1;
    }

    if (cfg.search) {
        const search::SearchLimits limits{cfg.search_depth};
        const search::SearchResult res =
            search::iterative_deepening(pos, mem, limits, &std::cout);

        std::cout << "bestmove " << search::move_to_uci(res.best_move) << "\n";
        memory_free(mem);
        return 0;
    }

    if (cfg.divide) {
        divide(pos, mem, cfg.perft_depth, std::cout, cfg.threads, cfg.live_divide);
        memory_free(mem);
        return 0;
    }

    const PerftBenchResult perft_res = benchmark_perft(pos, mem, cfg.perft_depth, cfg.threads);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "perft\n";
    std::cout << "  depth               : " << perft_res.depth << "\n";
    std::cout << "  nodes               : " << perft_res.nodes << "\n";
    std::cout << "  time                : " << perft_res.seconds << " s\n";
    std::cout << "  nps                 : " << perft_res.nps << "\n";
    std::cout << "  hash mb             : " << cfg.hash_mb << "\n";
    std::cout << "  threads             : " << perft_res.threads << "\n";
    std::cout << "  attack backend      : " << attack_backend_name() << "\n";

    memory_free(mem);
    return 0;
}

} // namespace valerain
