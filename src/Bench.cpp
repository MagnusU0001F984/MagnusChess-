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

#include "Attack.h"
#include "Bench.h"
#include "Common.h"
#include "Memory.h"
#include "MoveGen.h"
#include "Nnue.h"
#include "Perft.h"
#include "Position.h"
#include "Search.h"

#include <array>
#include <charconv>
#include <chrono>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <streambuf>

/* ===== 繁體中文註釋 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 實作詳情請參閱對應的 .h 標頭檔案。
 */

namespace magnus {

namespace {

constexpr std::string_view BENCH_SEPARATOR = "===========================";
constexpr int MAX_SEARCH_THREADS = 512;

constexpr std::array<std::string_view, 13> SEARCH_BENCH_FENS{{
    "1rbqk2r/p2pppbp/6p1/3QP3/8/4B3/PPP2PPP/2KR1B1R b k - 2 11",
    "r1bqk2r/p2pppbp/2p3p1/3NP3/8/4B3/PPP2PPP/R2QKB1R b KQkq - 0 9",
    "R4b2/5pkp/1Q2b1p1/1B6/8/8/P1P2PPP/1K2R3 b - - 2 21",
    "r1bqkb1r/pppp1ppp/2n5/1B6/3pn3/5N2/PPP2PPP/RNBQR1K1 b kq - 1 6",
    "r1bqkb1r/pppp2pp/8/1B3p2/3Qn3/8/PPP2PPP/RNB1R1K1 b kq - 0 8",
    "r3k2r/pp4pp/2p1b3/2b5/8/7P/P2q1PP1/1R4K1 w kq - 1 19",
    "r3kb1r/pp4pp/2p1b3/8/8/2N1B3/Pq3PPP/2R3K1 w kq - 0 16",
    "2rr1k2/4ppb1/2p3p1/p1P1PbNp/3N1BnP/P7/1P3PP1/2RR2K1 w - - 4 23",
    "r3kb1r/pp4pp/2p1b3/8/8/2q5/P2B1PPP/1R4K1 b kq - 1 17",
    "4k3/4ppb1/6p1/N1r1PbNp/5BnP/P7/1P3PP1/3R2K1 w - - 0 26",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r4rk1/ppp1qppp/1b3n2/8/3nP1b1/1BNN4/PPPP1PPP/R1B1QRK1 b - - 6 11",
    "r4rk1/ppp1qppp/1b3n2/8/4P3/1BNN1b1P/PPPP1P2/R1B1QRK1 b - - 0 13",
}};

struct SearchBenchResult {
    search::SearchResult search{};
    u64 time_ms = 0;
    u64 nps = 0;
    std::string ponder{};
};

[[nodiscard]] bool ensure_nnue_loaded(
    const std::string& eval_file,
    std::ostream* out
) {
    if (nnue::loaded() && nnue::path() == eval_file)
        return true;

    if (nnue::load(eval_file)) {
        if (out)
            *out << "info string loaded nnue " << eval_file << '\n';
        return true;
    }

    if (out)
        *out << "info string failed to load nnue: " << nnue::last_error() << '\n';
    return false;
}

[[nodiscard]] SearchBenchResult benchmark_search_position(
    const Position& pos,
    memory::Memory& mem,
    const search::SearchLimits& limits,
    std::ostream* out
) {
    using clock = std::chrono::steady_clock;

    memory::memory_clear_hash(mem);

    const auto start = clock::now();
    SearchBenchResult result;
    if (out != nullptr) {
        PvTrackingStreamBuf pv_tracking_buf(out->rdbuf());
        std::ostream tracked_out(&pv_tracking_buf);
        result.search = search::iterative_deepening(pos, mem, limits, &tracked_out);
        tracked_out.flush();
        result.ponder = ponder_move_from_last_pv(
            pos,
            mem,
            result.search.best_move,
            pv_tracking_buf.last_pv()
        );
    } else {
        result.search = search::iterative_deepening(pos, mem, limits, nullptr);
    }
    const auto end = clock::now();

    const u64 time_ms = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
    );
    const double seconds = std::chrono::duration<double>(end - start).count();

    result.time_ms = time_ms;
    result.nps = seconds > 0.0
        ? static_cast<u64>(static_cast<double>(result.search.nodes) / seconds)
        : 0ULL;
    return result;
}

void render_search_bench_summary(
    std::ostream& out,
    u64 total_time_ms,
    u64 total_nodes
) {
    const u64 total_nps = total_time_ms > 0
        ? static_cast<u64>((total_nodes * 1000ULL) / total_time_ms)
        : 0ULL;

    out << BENCH_SEPARATOR << "\n";
    out << "Total time (ms) : " << total_time_ms << "\n";
    out << "Nodes searched  : " << total_nodes << "\n";
    out << "Nodes/second    : " << total_nps << "\n";
}

} // namespace

/*
Bench mode is deliberately tiny: create a start position, initialize shared
tables, then route either to perft/divide or to the fixed-depth search smoke test.
*/

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

bool run_search_bench(
    memory::Memory& mem,
    int depth,
    std::size_t threads,
    bool use_nnue,
    bool emit_ponder,
    std::ostream& out
) {
    u64 total_time_ms = 0;
    u64 total_nodes = 0;

    const int search_threads = std::clamp<int>(static_cast<int>(threads), 1, MAX_SEARCH_THREADS);
    out << "info string Using " << search_threads << " thread"
        << (search_threads == 1 ? "" : "s") << "\n\n";

    for (std::size_t i = 0; i < SEARCH_BENCH_FENS.size(); ++i) {
        Position bench_pos{};
        const std::string_view fen = SEARCH_BENCH_FENS[i];
        if (!parse_fen(bench_pos, mem, fen)) {
            out << "info string failed to parse bench FEN: " << fen << '\n';
            return false;
        }

        out << "Position: " << (i + 1) << '/' << SEARCH_BENCH_FENS.size()
            << " (" << fen << ")\n";

        search::SearchLimits limits{};
        limits.depth = depth;
        limits.use_nnue = use_nnue;
        limits.thread_count = search_threads;
        limits.thread_id = 0;
        limits.report_info = true;

        const SearchBenchResult res = benchmark_search_position(
            bench_pos,
            mem,
            limits,
            &out
        );

        total_time_ms += res.time_ms;
        total_nodes += res.search.nodes;

        out << "bestmove " << search::move_to_uci(res.search.best_move);
        if (emit_ponder && !res.ponder.empty())
            out << " ponder " << res.ponder;
        out << "\n";
        if (i + 1 != SEARCH_BENCH_FENS.size())
            out << "\n";
    }

    render_search_bench_summary(out, total_time_ms, total_nodes);
    return true;
}

bool run_timed_search_bench(
    memory::Memory& mem,
    int movetime_ms,
    std::size_t threads,
    bool use_nnue,
    bool emit_ponder,
    std::ostream& out
) {
    u64 total_time_ms = 0;
    u64 total_nodes = 0;

    const int search_threads = std::clamp<int>(static_cast<int>(threads), 1, MAX_SEARCH_THREADS);
    out << "info string Using " << search_threads << " thread"
        << (search_threads == 1 ? "" : "s") << "\n\n";

    for (std::size_t i = 0; i < SEARCH_BENCH_FENS.size(); ++i) {
        Position bench_pos{};
        const std::string_view fen = SEARCH_BENCH_FENS[i];
        if (!parse_fen(bench_pos, mem, fen)) {
            out << "info string failed to parse bench FEN: " << fen << '\n';
            return false;
        }

        out << "Position: " << (i + 1) << '/' << SEARCH_BENCH_FENS.size()
            << " (" << fen << ")\n";

        search::SearchLimits limits{};
        limits.depth = search::MAX_PLY;
        limits.soft_time_ms = movetime_ms;
        limits.hard_time_ms = movetime_ms;
        limits.use_nnue = use_nnue;
        limits.thread_count = search_threads;
        limits.thread_id = 0;
        limits.report_info = true;

        const SearchBenchResult res = benchmark_search_position(
            bench_pos,
            mem,
            limits,
            &out
        );

        total_time_ms += res.time_ms;
        total_nodes += res.search.nodes;

        out << "bestmove " << search::move_to_uci(res.search.best_move);
        if (emit_ponder && !res.ponder.empty())
            out << " ponder " << res.ponder;
        out << "\n";
        if (i + 1 != SEARCH_BENCH_FENS.size())
            out << "\n";
    }

    render_search_bench_summary(out, total_time_ms, total_nodes);
    return true;
}

/*
 * 基準測試實作
 * parse_config() — 解析命令列參數（perft/search/timed_search 模式）
 * benchmark_perft() — Perft 節點計數基準測試
 * run_search_bench() — 固定深度搜尋基準測試
 * run_timed_search_bench() — 定時搜尋基準測試（模擬真實對局）
 */
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
    else if (argc > 1 && std::string_view(argv[1]) == "bench") {
        cfg.search = true;
        cfg.timed_search = true;
        argi = 2;
    }

    auto parse_arg_int = [&](int idx, int default_val) noexcept {
        if (idx >= argc || !argv[idx] || !argv[idx][0])
            return default_val;
        int value = 0;
        std::string_view sv(argv[idx]);
        if (std::from_chars(sv.data(), sv.data() + sv.size(), value).ec == std::errc{})
            return value;
        return default_val;
    };
    auto parse_arg_u64 = [&](int idx, std::size_t default_val) noexcept -> std::size_t {
        if (idx >= argc || !argv[idx] || !argv[idx][0])
            return default_val;
        u64 value = 0;
        std::string_view sv(argv[idx]);
        if (std::from_chars(sv.data(), sv.data() + sv.size(), value).ec == std::errc{})
            return static_cast<std::size_t>(value);
        return default_val;
    };

    if (cfg.search) {
        if (cfg.timed_search)
            cfg.search_movetime_ms = std::max(1, parse_arg_int(argi, cfg.search_movetime_ms));
        else
            cfg.search_depth = std::max(1, parse_arg_int(argi, cfg.search_depth));
        cfg.hash_mb = parse_arg_u64(argi + 1, cfg.hash_mb);
        cfg.threads = parse_arg_u64(argi + 2, cfg.threads);
    } else {
        cfg.perft_depth = std::max(0, parse_arg_int(argi, cfg.perft_depth));
        cfg.hash_mb = parse_arg_u64(argi + 1, cfg.hash_mb);
        cfg.threads = parse_arg_u64(argi + 2, cfg.threads);
        if (argc > argi + 3 && std::string_view(argv[argi + 3]) == "live")
            cfg.live_divide = true;
    }
    if (cfg.hash_mb == 0) cfg.hash_mb = 1;
    if (cfg.threads == 0) cfg.threads = 1;

    return cfg;
}

int run_bench(int argc, char** argv) { 
    const BenchConfig cfg = parse_config(argc, argv);
    bool use_nnue = false;

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
        if (cfg.timed_search) {
            const std::string eval_file = default_eval_file();
            use_nnue = ensure_nnue_loaded(eval_file, &std::cout);
            if (!use_nnue)
                std::cout << "info string nnue unavailable, bench will use hce\n";
        }

        const bool ok = cfg.timed_search
            ? run_timed_search_bench(
                mem,
                cfg.search_movetime_ms,
                cfg.threads,
                use_nnue,
                true,
                std::cout
            )
            : run_search_bench(
                mem,
                cfg.search_depth,
                cfg.threads,
                false,
                true,
                std::cout
            );
        memory_free(mem);
        return ok ? 0 : 1;
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

} // namespace magnus
