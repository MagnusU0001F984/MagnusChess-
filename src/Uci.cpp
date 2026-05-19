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

#include "Uci.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <limits>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "Attack.h"
#include "Bench.h"
#include "Common.h"
#include "Evaluation.h"
#include "Memory.h"
#include "Nnue.h"
#include "Search.h"
#include "Time.h"

/*
 * MagnusChess UCI 前端 — Universal Chess Interface 實作
 *
 * 完整的 UCI 協定實作，包含以下功能模組：
 *
 * 1. UCI 命令解析與派發
 *    - uci / isready / ucinewgame / quit — 標準 UCI 握手
 *    - position [startpos|fen ...] [moves ...] — 局面設定
 *    - go [depth|movetime|wtime|btime|...] — 搜尋控制
 *    - stop / ponderhit — 非同步搜尋控制
 *    - setoption name [Hash|Threads|EvalFile|...] value — 引擎選項
 *
 * 2. FEN 解析 (parse_fen)
 *    - 驗證棋盤結構、走子方、易位權、過路兵
 *    - 通過標準 piece-placement API 建構局面
 *
 * 3. 搜尋線程管理 (UciSession)
 *    - 合作式停止 (stop_requested 原子變量)
 *    - 沉思模式支援 (ponder / ponderhit 時間追蹤)
 *    - 背景 NNUE 預加載
 *
 * 4. PvTrackingStreamBuf — 自訂 streambuf
 *    - 攔截搜尋輸出的 "info ... pv ..." 行
 *    - 提取 PV 用於沉思著法計算
 */

namespace magnus {

namespace {

constexpr int DEFAULT_UCI_DEPTH = 8;
constexpr int DEFAULT_UCI_THREADS = 1;
constexpr int MAX_UCI_THREADS = 512;
constexpr int DEFAULT_UCI_CONTEMPT = 0;
constexpr int MIN_UCI_CONTEMPT = -10000;
constexpr int MAX_UCI_CONTEMPT = 10000;
struct PositionHistory {
    Key keys[search::MAX_GAME_HISTORY]{};
    int count = 0;
    int head = 0; // circular-buffer write cursor
};

inline void clear_position_history(PositionHistory& history) noexcept {
    history.count = 0;
    history.head = 0;
}

inline void push_position_history(
    PositionHistory& history,
    Key key
) noexcept {
    if (history.count < search::MAX_GAME_HISTORY) {
        history.keys[history.count] = key;
        ++history.count;
        return;
    }
    // Circular overwrite: O(1) instead of O(n) shift.
    history.keys[history.head] = key;
    history.head = (history.head + 1) % search::MAX_GAME_HISTORY;
}

[[nodiscard]] constexpr const char* go_usage_hint() noexcept {
    return "go <depth/movetime/nodes/ponder>";
}

[[nodiscard]] constexpr const char* go_usage_examples() noexcept {
    return "examples: go depth 8 | go movetime 1000 | go nodes 50000 | go wtime 15000 btime 15000 | go ponder wtime 15000 btime 15000";
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

[[nodiscard]] inline long long steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
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
        else if (token == "ponder") {
            params.ponder = true;
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
    bool enable_ponder = true;
    int threads = DEFAULT_UCI_THREADS;
    int contempt = DEFAULT_UCI_CONTEMPT;
    std::string eval_file = default_eval_file();
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> search_running{false};
    std::atomic<bool> ponder_search{false};
    std::atomic<bool> pondering{false};
    std::atomic<bool> ponder_hit_received{false};
    std::atomic<int> ponder_time_offset_ms{0};
    std::atomic<long long> search_start_ms{0};
    std::thread search_thread;
    std::thread nnue_preload_thread;

    UciSession() {
        memory::memory_init(mem, 64, 8, 2);
        // attack_init_backend deferred to first command that needs it.
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
        // Start NNUE preload in background so the file is ready by the first search.
        if (use_nnue && !eval_file.empty()) {
            nnue_preload_thread = std::thread([path = eval_file]() {
                nnue::load(path);
            });
        }
    }

    void ensure_attack_ready() {
        attack_init_backend(mem);
    }

    ~UciSession() {
        stop_search();
        if (nnue_preload_thread.joinable())
            nnue_preload_thread.join();
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
        ponder_search.store(false, std::memory_order_release);
        pondering.store(false, std::memory_order_release);
        ponder_hit_received.store(false, std::memory_order_release);
        ponder_time_offset_ms.store(0, std::memory_order_release);
        search_start_ms.store(0, std::memory_order_release);
        search_running.store(false, std::memory_order_release);
    }

    void handle_ponderhit() noexcept {
        if (!search_running.load(std::memory_order_acquire) ||
            !ponder_search.load(std::memory_order_acquire) ||
            !pondering.load(std::memory_order_acquire)) {
            return;
        }

        const long long start_ms = search_start_ms.load(std::memory_order_acquire);
        const long long now_ms = steady_now_ms();
        const long long elapsed = start_ms > 0 ? std::max(0LL, now_ms - start_ms) : 0LL;
        const int offset_ms = static_cast<int>(
            std::min<long long>(elapsed, std::numeric_limits<int>::max())
        );

        ponder_time_offset_ms.store(offset_ms, std::memory_order_release);
        ponder_hit_received.store(true, std::memory_order_release);
        pondering.store(false, std::memory_order_release);
    }

    void emit_banner(std::ostream& out) const {
        out << "MagnusChess\U0001F984 0.1.1 by the Magnus developer" << std::endl;
    }

    void emit_uci_id(std::ostream& out) const {
        out << "id name MagnusChess 0.1.1\n";
        out << "id author Magnus\n";
        out << "option name Hash type spin default 64 min 1 max 1048576\n";
        out << "option name Threads type spin default 1 min 1 max " << MAX_UCI_THREADS << "\n";
        out << "option name Contempt type spin default " << DEFAULT_UCI_CONTEMPT
            << " min " << MIN_UCI_CONTEMPT
            << " max " << MAX_UCI_CONTEMPT << "\n";
        out << "option name Clear Hash type button\n";
        out << "option name UseNNUE type check default true\n";
        out << "option name Ponder type check default true\n";
        out << "option name EvalFile type string default " << eval_file << '\n';
        out << "uciok" << std::endl;
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
        if (position_history.count < search::MAX_GAME_HISTORY) {
            for (int i = 0; i < position_history.count; ++i)
                limits.game_history_keys[i] = position_history.keys[i];
            return;
        }
        // Circular buffer wrap: copy [head..end] then [0..head-1].
        const int head = position_history.head;
        int dst = 0;
        for (int i = head; i < search::MAX_GAME_HISTORY; ++i)
            limits.game_history_keys[dst++] = position_history.keys[i];
        for (int i = 0; i < head; ++i)
            limits.game_history_keys[dst++] = position_history.keys[i];
    }

    [[nodiscard]] bool ensure_nnue_loaded(std::ostream* out) {
        // Wait for any background preload to finish before touching NNUE state.
        if (nnue_preload_thread.joinable())
            nnue_preload_thread.join();

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

    void handle_setoption(std::ostream& out, std::string_view command) {
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
            if (parse_int(value, mb) && mb > 0) {
                // Safety cap: prevent OOM crash from -fno-exceptions + huge allocation.
                constexpr std::size_t MAX_HASH_MB = 1ULL << 20; // 1 TB
                const std::size_t clamped_mb = std::min(
                    static_cast<std::size_t>(mb), MAX_HASH_MB);
                memory::tt_resize_mb(mem.tt, clamped_mb);
            }
        }
        else if (name == "Threads") {
            int parsed_threads = 0;
            if (parse_int(value, parsed_threads))
                threads = std::clamp(parsed_threads, 1, MAX_UCI_THREADS);
        }
        else if (name == "Contempt") {
            int parsed_contempt = 0;
            if (parse_int(value, parsed_contempt)) {
                const int clamped = std::clamp(parsed_contempt, MIN_UCI_CONTEMPT, MAX_UCI_CONTEMPT);
                if (contempt != clamped)
                    memory::memory_clear_hash(mem);
                contempt = clamped;
            }
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
                if (use_nnue && !ensure_nnue_loaded(&out))
                    out << "info string nnue unavailable, search will fall back to hce\n";
            }
        }
        else if (name == "Ponder") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                enable_ponder = parsed;
        }
        else if (name == "EvalFile") {
            if (!value.empty()) {
                std::error_code ec;
                if (!std::filesystem::exists(value, ec) || ec) {
                    out << "info string EvalFile not found: " << value << '\n';
                    return;
                }
                eval_file = value;
                memory::memory_clear_hash(mem);

                if (use_nnue && !ensure_nnue_loaded(&out))
                    out << "info string nnue unavailable, search will fall back to hce\n";
            }
        }
    }

    void ensure_search_eval_ready(
        std::ostream& out,
        const char* fallback_message
    ) {
        if (use_nnue &&
            !nnue::loaded() &&
            !ensure_nnue_loaded(&out)) {
            out << fallback_message << '\n';
        }
    }

    void handle_eval(std::ostream& out) {
        if (use_nnue && !nnue::loaded())
            (void)ensure_nnue_loaded(nullptr);

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
        ensure_attack_ready();
        if (line != "bench") {
            out << "info string usage: " << bench_usage_hint() << '\n';
            return;
        }

        ensure_search_eval_ready(out, "info string nnue unavailable, bench will use hce");

        if (!run_timed_search_bench(
                mem,
                DEFAULT_BENCH_MOVETIME_MS,
                static_cast<std::size_t>(threads),
                use_nnue,
                enable_ponder,
                out
            ))
            out << "info string bench failed\n";
    }

    void handle_position(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        if (!set_position_from_command(
                pos,
                mem,
                command_arguments(line, "position"),
                position_history
            ))
            out << "info string invalid position command\n";
    }

    void handle_go(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        search::SearchLimits limits{};
        if (!parse_go_command(pos, mem, time_manager, line, limits)) {
            out << "info string usage: " << go_usage_hint() << '\n';
            out << "info string " << go_usage_examples() << '\n';
            return;
        }

        ensure_search_eval_ready(out, "info string nnue unavailable, search will use hce");

        limits.use_nnue = use_nnue;
        limits.contempt = contempt;
        limits.stop = &stop_requested;
        limits.pondering = &pondering;
        limits.ponder_time_offset_ms = &ponder_time_offset_ms;
        limits.thread_count = threads;
        limits.thread_id = 0;
        limits.report_info = true;
        copy_history_to_limits(limits);
        stop_requested.store(false, std::memory_order_release);
        ponder_search.store(limits.ponder, std::memory_order_release);
        pondering.store(limits.ponder, std::memory_order_release);
        ponder_hit_received.store(false, std::memory_order_release);
        ponder_time_offset_ms.store(0, std::memory_order_release);
        search_start_ms.store(steady_now_ms(), std::memory_order_release);

        const Position root = pos;
        search_thread = std::thread([this, root, limits]() {
            PvTrackingStreamBuf pv_tracking_buf(std::cout.rdbuf());
            std::ostream tracked_out(&pv_tracking_buf);
            const auto search_start = std::chrono::steady_clock::now();
            const bool started_as_ponder = limits.ponder;
            const search::SearchResult result =
                search::iterative_deepening(root, mem, limits, &tracked_out);
            tracked_out.flush();
            const auto search_end = std::chrono::steady_clock::now();
            const int wall_elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    search_end - search_start
                ).count()
            );
            const bool ponder_was_hit =
                !started_as_ponder || ponder_hit_received.load(std::memory_order_acquire);
            const int record_elapsed_ms = started_as_ponder
                ? std::max(
                    0,
                    wall_elapsed_ms - ponder_time_offset_ms.load(std::memory_order_acquire)
                )
                : wall_elapsed_ms;
            if (ponder_was_hit)
                time_manager.record_search(root, limits, result, record_elapsed_ms);

            const std::string ponder = ponder_move_from_last_pv(
                root,
                mem,
                result.best_move,
                pv_tracking_buf.last_pv()
            );

            std::cout << "bestmove " << search::move_to_uci(result.best_move);
            if (enable_ponder && !ponder.empty())
                std::cout << " ponder " << ponder;
            std::cout << std::endl;
            ponder_search.store(false, std::memory_order_release);
            pondering.store(false, std::memory_order_release);
            ponder_hit_received.store(false, std::memory_order_release);
            ponder_time_offset_ms.store(0, std::memory_order_release);
            search_start_ms.store(0, std::memory_order_release);
            search_running.store(false, std::memory_order_release);
        });
        search_running.store(true, std::memory_order_release);
    }

    void handle_perft(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        int depth = -1;
        std::size_t perft_threads = 0;
        bool live = false;
        if (!parse_divide_command(line, depth, perft_threads, live)) {
            out << divide_usage_hint() << '\n';
            return;
        }

        divide(pos, mem, depth, out, perft_threads, true);
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

        if (line == "ponderhit") {
            handle_ponderhit();
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
            handle_setoption(out, line);
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

    constexpr std::size_t MAX_UCI_LINE = 8192;
    std::string line;
    line.reserve(MAX_UCI_LINE);
    while (true) {
        line.clear();
        int ch = 0;
        while ((ch = std::cin.get()) != std::char_traits<char>::eof() && ch != '\n') {
            if (line.size() < MAX_UCI_LINE)
                line.push_back(static_cast<char>(ch));
        }
        if (ch == std::char_traits<char>::eof() && line.empty())
            break;
        if (line.size() >= MAX_UCI_LINE)
            std::cout << "info string line too long, ignoring\n";
        else if (!session.process_command(line, std::cout))
            break;
    }

    return 0;
}

} // namespace magnus
