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
#include "Nnue.h"
#include "Perft.h"
#include "Position.h"
#include "Search.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace valerain {

namespace {

constexpr std::string_view BENCH_SEPARATOR = "===========================";
constexpr int MAX_SEARCH_THREADS = 512;

constexpr std::array<std::string_view, 8> SEARCH_BENCH_FENS{{
    "1rbqk2r/p2pppbp/6p1/3QP3/8/4B3/PPP2PPP/2KR1B1R b k - 2 11",
    "r1bqk2r/p2pppbp/2p3p1/3NP3/8/4B3/PPP2PPP/R2QKB1R b KQkq - 0 9",
    "R4b2/5pkp/1Q2b1p1/1B6/8/8/P1P2PPP/1K2R3 b - - 2 21",
    "r1bqkb1r/pppp1ppp/2n5/1B6/3pn3/5N2/PPP2PPP/RNBQR1K1 b kq - 1 6",
    "r1bqkb1r/pppp2pp/8/1B3p2/3Qn3/8/PPP2PPP/RNB1R1K1 b kq - 0 8",
    "r3k2r/pp4pp/2p1b3/2b5/8/7P/P2q1PP1/1R4K1 w kq - 1 19",
    "r3kb1r/pp4pp/2p1b3/8/8/2N1B3/Pq3PPP/2R3K1 w kq - 0 16",
    "r3kb1r/pp4pp/2p1b3/8/8/2q5/P2B1PPP/1R4K1 b kq - 1 17"
}};

struct SearchBenchResult {
    search::SearchResult search{};
    u64 time_ms = 0;
    u64 nps = 0;
};

[[nodiscard]] std::string default_eval_file() {
    constexpr const char* candidates[] = {
        "Evalfile.bin",
        "bin/Evalfile.bin",
        "src/bin/Evalfile.bin",
        "src/Evalfile.bin",
        "src/bin/quantised.bin",
        "NnueFile/nn-2a5d6101d177.nnue",
        "src/NnueFile/nn-2a5d6101d177.nnue",
        "NnueFile/nn-37f18f62d772.nnue",
        "src/NnueFile/nn-37f18f62d772.nnue"
    };

    for (const char* candidate : candidates)
        if (std::filesystem::exists(candidate))
            return candidate;

    return "Evalfile.bin";
}

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

[[nodiscard]] bool parse_int(std::string_view sv, int& value) noexcept {
    const char* first = sv.data();
    const char* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc{} && ptr == last;
}

[[nodiscard]] bool parse_square(std::string_view sv, Square& sq) noexcept {
    if (sv.size() != 2)
        return false;

    const char file = sv[0];
    const char rank = sv[1];
    if (file < 'a' || file > 'h' || rank < '1' || rank > '8')
        return false;

    sq = static_cast<Square>((rank - '1') * 8 + (file - 'a'));
    return true;
}

[[nodiscard]] bool parse_piece_char(
    char c,
    Color& color,
    PieceType& piece_type
) noexcept {
    color = std::isupper(static_cast<unsigned char>(c)) ? WHITE : BLACK;

    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(c)))) {
        case 'p': piece_type = PAWN; return true;
        case 'n': piece_type = KNIGHT; return true;
        case 'b': piece_type = BISHOP; return true;
        case 'r': piece_type = ROOK; return true;
        case 'q': piece_type = QUEEN; return true;
        case 'k': piece_type = KING; return true;
        default: return false;
    }
}

[[nodiscard]] bool parse_fen(
    Position& pos,
    const memory::Memory& mem,
    std::string_view fen
) noexcept {
    std::istringstream iss{std::string(fen)};

    std::string board_part;
    std::string stm_part;
    std::string castling_part;
    std::string ep_part;
    std::string halfmove_part = "0";
    std::string fullmove_part = "1";

    if (!(iss >> board_part >> stm_part >> castling_part >> ep_part))
        return false;

    iss >> halfmove_part >> fullmove_part;

    position_clear(pos);

    int rank = 7;
    int file = 0;

    for (char c : board_part) {
        if (c == '/') {
            if (file != 8 || rank == 0)
                return false;

            --rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8)
                return false;
            continue;
        }

        Color color = WHITE;
        PieceType piece_type = PAWN;
        if (!parse_piece_char(c, color, piece_type) || file >= 8)
            return false;

        position_put_piece(pos, color, piece_type, rank * 8 + file);
        ++file;
    }

    if (rank != 0 || file != 8)
        return false;

    if (stm_part == "w") pos.side_to_move = WHITE;
    else if (stm_part == "b") pos.side_to_move = BLACK;
    else return false;

    pos.castling_rights = NO_CASTLING;
    if (castling_part != "-") {
        for (char c : castling_part) {
            switch (c) {
                case 'K': pos.castling_rights |= WHITE_OO; break;
                case 'Q': pos.castling_rights |= WHITE_OOO; break;
                case 'k': pos.castling_rights |= BLACK_OO; break;
                case 'q': pos.castling_rights |= BLACK_OOO; break;
                default: return false;
            }
        }
    }

    pos.ep_sq = NO_SQ;
    if (ep_part != "-" && !parse_square(ep_part, pos.ep_sq))
        return false;

    if (!parse_int(halfmove_part, pos.halfmove_clock) || pos.halfmove_clock < 0)
        return false;

    if (!parse_int(fullmove_part, pos.fullmove_number) || pos.fullmove_number <= 0)
        return false;

    position_refresh_key(pos, mem.tables);
    return position_has_valid_kings(pos) && position_board_matches_bitboards(pos);
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
    const search::SearchResult res = search::iterative_deepening(pos, mem, limits, out);
    const auto end = clock::now();

    const u64 time_ms = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
    );
    const double seconds = std::chrono::duration<double>(end - start).count();

    SearchBenchResult result;
    result.search = res;
    result.time_ms = time_ms;
    result.nps = seconds > 0.0
        ? static_cast<u64>(static_cast<double>(res.nodes) / seconds)
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

        out << "bestmove " << search::move_to_uci(res.search.best_move) << "\n";
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

        out << "bestmove " << search::move_to_uci(res.search.best_move) << "\n";
        if (i + 1 != SEARCH_BENCH_FENS.size())
            out << "\n";
    }

    render_search_bench_summary(out, total_time_ms, total_nodes);
    return true;
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
    else if (argc > 1 && std::string_view(argv[1]) == "bench") {
        cfg.search = true;
        cfg.timed_search = true;
        argi = 2;
    }

    if (cfg.search) {
        if (cfg.timed_search) {
            if (argc > argi)
                cfg.search_movetime_ms = std::max(1, std::atoi(argv[argi]));
        } else {
            if (argc > argi)
                cfg.search_depth = std::max(1, std::atoi(argv[argi]));
        }
        if (argc > argi + 1) cfg.hash_mb = static_cast<std::size_t>(std::strtoull(argv[argi + 1], nullptr, 10));
        if (argc > argi + 2) cfg.threads = static_cast<std::size_t>(std::strtoull(argv[argi + 2], nullptr, 10));
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
            ? run_timed_search_bench(mem, cfg.search_movetime_ms, cfg.threads, use_nnue, std::cout)
            : run_search_bench(mem, cfg.search_depth, cfg.threads, false, std::cout);
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

} // namespace valerain
