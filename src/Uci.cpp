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

#include "Uci.h"

#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "Attack.h"
#include "Bench.h"
#include "Evaluation.h"
#include "Memory.h"
#include "MoveGen.h"
#include "Nnue.h"
#include "Search.h"
#include "Time.h"

/*
This file implements the engine's minimal UCI front-end. It parses commands,
builds positions from startpos/FEN input, manages hash and NNUE options, and
derives practical search limits from the UCI go command.
*/

namespace valerain {

namespace {

constexpr int DEFAULT_UCI_DEPTH = 8;
constexpr int DEFAULT_BENCH_MOVETIME_MS = 1000;

struct PositionHistory {
    Key keys[search::MAX_GAME_HISTORY]{};
    int count = 0;
};

inline void clear_position_history(PositionHistory& history) noexcept {
    history.count = 0;
}

inline void push_position_history(
    PositionHistory& history,
    Key key
) noexcept {
    if (history.count < search::MAX_GAME_HISTORY) {
        history.keys[history.count++] = key;
        return;
    }

    for (int i = 1; i < search::MAX_GAME_HISTORY; ++i)
        history.keys[i - 1] = history.keys[i];
    history.keys[search::MAX_GAME_HISTORY - 1] = key;
}

[[nodiscard]] constexpr const char* go_usage_hint() noexcept {
    return "go <depth/movetime/nodes>";
}

[[nodiscard]] constexpr const char* go_usage_examples() noexcept {
    return "examples: go depth 8 | go movetime 1000 | go nodes 50000 | go wtime 15000 btime 15000";
}

[[nodiscard]] constexpr const char* perft_usage_hint() noexcept {
    return "perft <depth> <threads>";
}

[[nodiscard]] constexpr const char* divide_usage_hint() noexcept {
    return "divide <depth> <threads> [live]";
}

[[nodiscard]] constexpr const char* bench_usage_hint() noexcept {
    return "bench";
}

[[nodiscard]] bool command_starts_with(
    std::string_view line,
    std::string_view command
) noexcept {
    return line == command ||
           (line.size() > command.size() &&
            line.substr(0, command.size()) == command &&
            line[command.size()] == ' ');
}

[[nodiscard]] std::string_view command_arguments(
    std::string_view line,
    std::string_view command
) noexcept {
    return line.size() > command.size() ? line.substr(command.size() + 1) : std::string_view{};
}

[[nodiscard]] std::string default_eval_file() {
    // Prefer the repo-root bundled default, then fall back to in-tree legacy paths.
    constexpr const char* preferred = "Evalfile.bin";
    if (std::filesystem::exists(preferred))
        return preferred;

    constexpr const char* candidates[] = {
        "src/bin/quantised.bin",
        "NnueFile/nn-2a5d6101d177.nnue",
        "src/NnueFile/nn-2a5d6101d177.nnue",
        "NnueFile/nn-37f18f62d772.nnue",
        "src/NnueFile/nn-37f18f62d772.nnue"
    };

    for (const char* candidate : candidates)
        if (std::filesystem::exists(candidate))
            return candidate;

    return preferred;
}

[[nodiscard]] bool parse_int(std::string_view sv, int& value) noexcept {
    const char* first = sv.data();
    const char* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc{} && ptr == last;
}

[[nodiscard]] bool parse_u64(std::string_view sv, u64& value) noexcept {
    const char* first = sv.data();
    const char* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc{} && ptr == last;
}

[[nodiscard]] bool parse_bool(std::string_view sv, bool& value) noexcept {
    if (sv == "true" || sv == "1") {
        value = true;
        return true;
    }

    if (sv == "false" || sv == "0") {
        value = false;
        return true;
    }

    return false;
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
    // FEN pieces use case to encode color and the letter to encode piece type.
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

[[nodiscard]] bool move_matches_uci(Move move, std::string_view token) noexcept {
    if (token.size() != 4 && token.size() != 5)
        return false;

    if (token[0] != static_cast<char>('a' + file_of(from_sq(move))) ||
        token[1] != static_cast<char>('1' + rank_of(from_sq(move))) ||
        token[2] != static_cast<char>('a' + file_of(to_sq(move))) ||
        token[3] != static_cast<char>('1' + rank_of(to_sq(move)))) {
        return false;
    }

    if (!move_is_promotion(move))
        return token.size() == 4;

    if (token.size() != 5)
        return false;

    switch (promo_piece(move)) {
        case KNIGHT: return token[4] == 'n';
        case BISHOP: return token[4] == 'b';
        case ROOK:   return token[4] == 'r';
        case QUEEN:  return token[4] == 'q';
        default:     return false;
    }
}

[[nodiscard]] bool find_uci_move(
    const Position& pos,
    const memory::Memory& mem,
    std::string_view token,
    Move& move
) noexcept {
    // UCI moves are matched by generating legal moves and comparing the text form.
    MoveList list{};
    generate_legal(pos, mem, list);

    for (int i = 0; i < list.size; ++i) {
        if (move_matches_uci(list.moves[i], token)) {
            move = list.moves[i];
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool parse_fen(
    Position& pos,
    const memory::Memory& mem,
    const std::string& fen
) noexcept {
    // FEN parsing rebuilds the position through the regular piece-placement API
    // so mailbox, bitboards, eval caches, and king-square caches all stay aligned.
    std::istringstream iss(fen);

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

[[nodiscard]] bool apply_move_list(
    Position& pos,
    const memory::Memory& mem,
    std::istringstream& iss,
    PositionHistory& history
) noexcept {
    std::string move_token;
    while (iss >> move_token) {
        Move move = 0;
        if (!find_uci_move(pos, mem, move_token, move))
            return false;

        const Key prev_key = pos.key;
        do_move_copy(pos, move, mem.tables);
        if (pos.halfmove_clock == 0)
            clear_position_history(history);
        else
            push_position_history(history, prev_key);
    }

    return true;
}

[[nodiscard]] bool set_position_from_command(
    Position& pos,
    const memory::Memory& mem,
    std::string_view command,
    PositionHistory& history
) noexcept {
    std::istringstream iss{std::string(command)};
    std::string token;

    if (!(iss >> token))
        return false;

    if (token == "startpos") {
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
    } else if (token == "fen") {
        std::string fen;
        std::string part;

        for (int i = 0; i < 6; ++i) {
            if (!(iss >> part))
                return false;

            if (i != 0)
                fen.push_back(' ');
            fen += part;
        }

        if (!parse_fen(pos, mem, fen))
            return false;
    } else {
        return false;
    }

    clear_position_history(history);

    if (!(iss >> token))
        return true;

    if (token != "moves")
        return false;

    return apply_move_list(pos, mem, iss, history);
}

[[nodiscard]] bool ensure_nnue_loaded(
    const std::string& eval_file,
    std::ostream* out
) {
    // Load lazily so HCE remains usable even when no NNUE file is configured.
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

[[nodiscard]] const char* active_eval_name(bool use_nnue) noexcept {
    return use_nnue && nnue::loaded() ? "nnue" : "hce";
}

[[nodiscard]] inline int white_pov_score(const Position& pos, int stm_score) noexcept {
    return pos.side_to_move == WHITE ? stm_score : -stm_score;
}

[[nodiscard]] inline int white_pov_winrate(const Position& pos, int stm_wr) noexcept {
    return pos.side_to_move == WHITE ? stm_wr : (1000 - stm_wr);
}

[[nodiscard]] inline nnue::WdlTriplet white_pov_wdl(
    const Position& pos,
    nnue::WdlTriplet stm_wdl
) noexcept {
    if (pos.side_to_move == WHITE)
        return stm_wdl;

    return {
        .win = stm_wdl.loss,
        .draw = stm_wdl.draw,
        .loss = stm_wdl.win
    };
}

void handle_setoption(
    memory::Memory& mem,
    bool& use_nnue,
    std::string& eval_file,
    std::ostream& out,
    std::string_view command
) {
    // Option parsing is intentionally small: hash size, hash clear, NNUE toggle,
    // and NNUE file path are all that the engine currently exposes.
    std::istringstream iss{std::string(command)};
    std::string token;
    std::string name;
    std::string value;

    iss >> token; // setoption
    iss >> token; // name

    while (iss >> token) {
        if (token == "value")
            break;

        if (!name.empty())
            name.push_back(' ');
        name += token;
    }

    std::getline(iss, value);
    if (!value.empty() && value.front() == ' ')
        value.erase(value.begin());

    if (name == "Hash") {
        int mb = 0;
        if (parse_int(value, mb) && mb > 0)
            memory::tt_resize_mb(mem.tt, static_cast<std::size_t>(mb));
    }
    else if (name == "Clear Hash") {
        memory::memory_clear_hash(mem);
    }
    else if (name == "UseNNUE") {
        bool parsed = false;
        if (parse_bool(value, parsed)) {
            if (use_nnue != parsed)
                memory::memory_clear_hash(mem);

            use_nnue = parsed;
            if (use_nnue && !ensure_nnue_loaded(eval_file, &out))
                out << "info string nnue unavailable, search will fall back to hce\n";
        }
    }
    else if (name == "EvalFile") {
        if (!value.empty()) {
            eval_file = value;
            memory::memory_clear_hash(mem);

            if (use_nnue && !ensure_nnue_loaded(eval_file, &out))
                out << "info string nnue unavailable, search will fall back to hce\n";
        }
    }
}

[[nodiscard]] bool parse_go_command(
    const Position& pos,
    const memory::Memory& mem,
    timeman::TimeManager& time_manager,
    std::string_view command,
    search::SearchLimits& limits
) noexcept {
    // Convert the UCI go command into normalized parameters, then let the time
    // manager derive final soft/hard budgets (with historical adjustment).
    std::istringstream iss{std::string(command)};
    std::string token;

    timeman::GoParams params{};
    bool has_limit = false;
    limits.root_move_count = 0;

    iss >> token; // go

    const auto parse_next_int = [&](int& value, bool require_positive = false) noexcept {
        std::string raw;
        return (iss >> raw) &&
               parse_int(raw, value) &&
               (!require_positive || value > 0);
    };

    const auto parse_next_u64 = [&](u64& value, bool require_positive = false) noexcept {
        std::string raw;
        return (iss >> raw) &&
               parse_u64(raw, value) &&
               (!require_positive || value > 0);
    };

    while (iss >> token) {
        if (token == "depth") {
            if (!parse_next_int(params.depth, true))
                return false;
            has_limit = true;
        }
        else if (token == "nodes") {
            if (!parse_next_u64(params.nodes, true))
                return false;
            has_limit = true;
        }
        else if (token == "movetime") {
            if (!parse_next_int(params.movetime, true))
                return false;
            has_limit = true;
        }
        else if (token == "wtime") {
            if (!parse_next_int(params.wtime))
                return false;
            has_limit = true;
        }
        else if (token == "btime") {
            if (!parse_next_int(params.btime))
                return false;
            has_limit = true;
        }
        else if (token == "winc") {
            if (!parse_next_int(params.winc))
                return false;
            has_limit = true;
        }
        else if (token == "binc") {
            if (!parse_next_int(params.binc))
                return false;
            has_limit = true;
        }
        else if (token == "movestogo") {
            if (!parse_next_int(params.movestogo))
                return false;
            has_limit = true;
        }
        else if (token == "infinite") {
            params.infinite = true;
            has_limit = true;
        }
        else if (token == "searchmoves") {
            std::string move_token;
            while (iss >> move_token) {
                Move move = 0;
                if (!find_uci_move(pos, mem, move_token, move))
                    return false;

                bool duplicate = false;
                for (int i = 0; i < limits.root_move_count; ++i) {
                    if (limits.root_moves[i] == move) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;

                if (limits.root_move_count >= 256)
                    return false;
                limits.root_moves[limits.root_move_count++] = move;
            }

            if (limits.root_move_count == 0)
                return false;
            break;
        }
    }

    if (!has_limit)
        return false;

    return time_manager.build_limits(pos, params, limits);
}

[[nodiscard]] bool parse_divide_command(
    std::string_view command,
    int& depth,
    std::size_t& threads,
    bool& live
) noexcept {
    std::istringstream iss{std::string(command)};
    std::string token;
    std::string value;

    depth = -1;
    threads = 0;
    live = false;

    iss >> token; // divide

    const auto parse_next_int = [&](int& parsed, bool require_positive) noexcept {
        return (iss >> value) &&
               parse_int(value, parsed) &&
               (!require_positive || parsed > 0);
    };

    if (!parse_next_int(depth, false) || depth < 0)
        return false;

    int parsed_threads = 0;
    if (!parse_next_int(parsed_threads, true))
        return false;

    threads = static_cast<std::size_t>(parsed_threads);

    if (iss >> value) {
        if (value == "live")
            live = true;
        else
            return false;
    }

    return true;
}

struct UciSession {
    memory::Memory mem{};
    Position pos{};
    PositionHistory position_history{};
    timeman::TimeManager time_manager{};
    bool use_nnue = true;
    std::string eval_file = default_eval_file();
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> search_running{false};
    std::thread search_thread;

    UciSession() {
        memory::memory_init(mem, 64, 8, 2);
        attack_init_backend(mem);
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
    }

    ~UciSession() {
        stop_search();
        memory::memory_free(mem);
    }

    void join_finished_search() {
        if (search_thread.joinable() &&
            !search_running.load(std::memory_order_acquire)) {
            search_thread.join();
        }
    }

    void stop_search() {
        stop_requested.store(true, std::memory_order_release);
        if (search_thread.joinable())
            search_thread.join();
        search_running.store(false, std::memory_order_release);
    }

    void emit_banner(std::ostream& out) const {
        out << "Valerain 0.0.8 by the Magnus developer" << std::endl;
    }

    void emit_uci_id(std::ostream& out) const {
        out << "id name Valerain 0.0.8\n";
        out << "id author Magnus\n";
        out << "option name Hash type spin default 64 min 1 max 33554432\n";
        out << "option name Clear Hash type button\n";
        out << "option name UseNNUE type check default true\n";
        out << "option name EvalFile type string default " << eval_file << '\n';
        out << "uciok\n";
    }

    void reset_new_game() {
        memory::memory_clear_hash(mem);
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
        clear_position_history(position_history);
        time_manager.new_game();
    }

    void copy_history_to_limits(search::SearchLimits& limits) const {
        limits.game_history_count = position_history.count;
        for (int i = 0; i < position_history.count; ++i)
            limits.game_history_keys[i] = position_history.keys[i];
    }

    void ensure_search_eval_ready(
        std::ostream& out,
        const char* fallback_message
    ) {
        if (use_nnue &&
            !nnue::loaded() &&
            !ensure_nnue_loaded(eval_file, &out)) {
            out << fallback_message << '\n';
        }
    }

    void handle_eval(std::ostream& out) {
        if (use_nnue && !nnue::loaded())
            (void)ensure_nnue_loaded(eval_file, nullptr);

        out << "info string eval " << active_eval_name(use_nnue) << '\n';
        const int hce_stm = eval::evaluate(pos);
        out << "info string hce cp " << white_pov_score(pos, hce_stm) << '\n';

        if (nnue::loaded()) {
            const int raw_stm = nnue::eval(pos);
            const int cp_stm = nnue::to_cp(raw_stm, pos);
            const int winrate_stm = nnue::win_rate_model(raw_stm, pos);
            const int search_stm = nnue::search_score(raw_stm, pos);
            const int search_cp_stm = nnue::search_score_to_cp(search_stm, pos);
            const nnue::WdlTriplet wdl_white =
                white_pov_wdl(pos, nnue::search_score_to_wdl(search_stm, pos));

            out << "info string nnue raw " << white_pov_score(pos, raw_stm) << '\n';
            out << "info string nnue cp " << white_pov_score(pos, cp_stm) << '\n';
            out << "info string nnue search " << white_pov_score(pos, search_stm) << '\n';
            out << "info string nnue searchcp " << white_pov_score(pos, search_cp_stm) << '\n';
            out << "info string nnue winrate "
                << white_pov_winrate(pos, winrate_stm) << '\n';
            out << "info string nnue wdl "
                << wdl_white.win << ' '
                << wdl_white.draw << ' '
                << wdl_white.loss << '\n';
        } else {
            out << "info string nnue unavailable\n";
        }
    }

    void handle_bench(std::string_view line, std::ostream& out) {
        if (line != "bench") {
            out << "info string usage: " << bench_usage_hint() << '\n';
            return;
        }

        ensure_search_eval_ready(out, "info string nnue unavailable, bench will use hce");

        if (!run_timed_search_bench(
                mem,
                DEFAULT_BENCH_MOVETIME_MS,
                use_nnue,
                out
            ))
            out << "info string bench failed\n";
    }

    void handle_position(std::string_view line, std::ostream& out) {
        if (!set_position_from_command(
                pos,
                mem,
                command_arguments(line, "position"),
                position_history
            ))
            out << "info string invalid position command\n";
    }

    void handle_go(std::string_view line, std::ostream& out) {
        search::SearchLimits limits{};
        if (!parse_go_command(pos, mem, time_manager, line, limits)) {
            out << "info string usage: " << go_usage_hint() << '\n';
            out << "info string " << go_usage_examples() << '\n';
            return;
        }

        ensure_search_eval_ready(out, "info string nnue unavailable, search will use hce");

        limits.use_nnue = use_nnue;
        limits.stop = &stop_requested;
        copy_history_to_limits(limits);
        stop_requested.store(false, std::memory_order_release);

        const Position root = pos;
        search_running.store(true, std::memory_order_release);
        search_thread = std::thread([this, root, limits]() {
            const auto search_start = std::chrono::steady_clock::now();
            const search::SearchResult result =
                search::iterative_deepening(root, mem, limits, &std::cout);
            const auto search_end = std::chrono::steady_clock::now();
            const int elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    search_end - search_start
                ).count()
            );
            time_manager.record_search(root, limits, result, elapsed_ms);

            std::cout << "bestmove " << search::move_to_uci(result.best_move) << '\n';
            search_running.store(false, std::memory_order_release);
        });
    }

    void handle_perft(std::string_view line, std::ostream& out) {
        int depth = -1;
        std::size_t threads = 0;
        bool live = false;
        if (!parse_divide_command(line, depth, threads, live)) {
            out << divide_usage_hint() << '\n';
            return;
        }

        divide(pos, mem, depth, out, threads, true);
    }

    [[nodiscard]] bool process_command(
        std::string_view line,
        std::ostream& out
    ) {
        join_finished_search();

        if (line == "uci") {
            emit_uci_id(out);
            return true;
        }

        if (line == "isready") {
            out << "readyok\n";
            return true;
        }

        if (line == "stop") {
            stop_search();
            return true;
        }

        if (line == "quit") {
            stop_search();
            return false;
        }

        if (search_running.load(std::memory_order_acquire)) {
            out << "info string search busy, send stop first\n";
            return true;
        }

        if (line == "ucinewgame") {
            reset_new_game();
            return true;
        }

        if (command_starts_with(line, "setoption")) {
            handle_setoption(mem, use_nnue, eval_file, out, line);
            return true;
        }

        if (line == "eval") {
            handle_eval(out);
            return true;
        }

        if (command_starts_with(line, "bench")) {
            handle_bench(line, out);
            return true;
        }

        if (command_starts_with(line, "position")) {
            handle_position(line, out);
            return true;
        }

        if (command_starts_with(line, "go")) {
            handle_go(line, out);
            return true;
        }

        if (command_starts_with(line, "perft")) {
            handle_perft(line, out);
            return true;
        }

        return true;
    }
};

} // namespace

int run_uci() {
    // UCI command loop with cooperative stop support.
    std::cout << std::unitbuf;
    UciSession session{};
    session.emit_banner(std::cout);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!session.process_command(line, std::cout))
            break;
    }

    return 0;
}

} // namespace valerain
