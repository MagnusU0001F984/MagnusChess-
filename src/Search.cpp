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

#include "Search.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>

#include "Attack.h"
#include "Evaluation.h"
#include "MoveGen.h"
#include "Nnue.h"

/*
This file implements a compact classical search:
- iterative deepening at the root
- principal variation search in the main tree
- quiescence search on the tactical frontier
- transposition-table guided ordering and cutoffs
- a restrained set of low-overhead pruning heuristics
*/

namespace valerain::search {

namespace {

// Small fixed margins keep the implementation simple and the runtime overhead low.
constexpr int VALUE_INF = 32000;
constexpr int VALUE_MATE = 31000;
constexpr int DELTA_MARGIN = 200;
constexpr int FUTILITY_MARGIN[3] = { 0, 120, 240 };
constexpr int REVERSE_FUTILITY_MARGIN[4] = { 0, 90, 180, 300 };
constexpr int RAZOR_MARGIN[3] = { 0, 280, 420 };
constexpr int ASPIRATION_DELTA = 24;
constexpr int REPETITION_AVOID_SCORE = -16;
constexpr int IIR_MIN_DEPTH = 6;
constexpr int SEE_PRUNE_DEPTH_LIMIT = 6;

constexpr int piece_order_value[PIECE_TYPE_NB] = {
    100, 320, 330, 500, 900, 0
};

constexpr int see_piece_value[PIECE_TYPE_NB] = {
    100, 320, 330, 500, 900, 20000
};

/*
Searcher owns the mutable state for one iterative-deepening session: node
counting, killer/history tables, PV storage, and stop-condition bookkeeping.
*/
struct Searcher {
    using clock = std::chrono::steady_clock;

    memory::Memory& mem;
    const SearchLimits& limits;

    u64 nodes = 0;
    u64 base_nodes = 0;
    Move killers[MAX_PLY][2]{};
    i32 history[COLOR_NB][PIECE_TYPE_NB][SQ_NB]{};
    Move pv_table[MAX_PLY][MAX_PLY]{};
    int pv_length[MAX_PLY + 1]{};
    Key rep_keys[MAX_PLY + 1]{};
    clock::time_point start_time{};
    bool stopped = false;
    bool hard_stop = false;

    explicit Searcher(memory::Memory& m, const SearchLimits& l) noexcept
        : mem(m), limits(l) {}

    // Mate scores are stored relative to the current ply so TT hits remain
    // consistent when reused at a different depth from the root.
    [[nodiscard]] static inline int score_to_tt(int score, int ply) noexcept {
        if (score >= VALUE_MATE - MAX_PLY)
            return score + ply;
        if (score <= -VALUE_MATE + MAX_PLY)
            return score - ply;
        return score;
    }

    [[nodiscard]] static inline int score_from_tt(int score, int ply) noexcept {
        if (score >= VALUE_MATE - MAX_PLY)
            return score - ply;
        if (score <= -VALUE_MATE + MAX_PLY)
            return score + ply;
        return score;
    }

    [[nodiscard]] static inline memory::Bound bound_from_score(
        int score,
        int alpha,
        int beta
    ) noexcept {
        if (score <= alpha)
            return memory::BOUND_UPPER;
        if (score >= beta)
            return memory::BOUND_LOWER;
        return memory::BOUND_EXACT;
    }

    [[nodiscard]] static inline bool is_mate_window(int score) noexcept {
        return score <= -VALUE_MATE + MAX_PLY || score >= VALUE_MATE - MAX_PLY;
    }

    // Delta pruning only needs a rough upper bound on how much a capture can gain.
    [[nodiscard]] static inline int capture_gain_estimate(
        const Position& pos,
        Move move
    ) noexcept {
        int gain = move_is_ep(move)
            ? piece_order_value[PAWN]
            : piece_order_value[piece_type_on(pos, to_sq(move))];

        if (move_is_promotion(move))
            gain += piece_order_value[promo_piece(move)] - piece_order_value[PAWN];

        return gain;
    }

    // Fixed schedules avoid expensive formulas for lightweight pruning.
    [[nodiscard]] static inline int lmr_reduction(int depth, int move_index) noexcept {
        if (depth >= 6 && move_index >= 6)
            return 2;
        return 1;
    }

    [[nodiscard]] static inline int null_reduction(int depth) noexcept {
        return depth >= 6 ? 3 : 2;
    }

    [[nodiscard]] static inline int lmp_limit(int depth) noexcept {
        return 2 + depth * 3;
    }

    [[nodiscard]] static inline int history_prune_threshold(int depth) noexcept {
        return -depth * depth * 4;
    }

    [[nodiscard]] inline u64 global_nodes() const noexcept {
        return base_nodes + nodes;
    }

    [[nodiscard]] inline int elapsed_ms() const noexcept {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - start_time
        ).count());
    }

    [[nodiscard]] inline bool hit_hard_limit() noexcept {
        if (stopped)
            return true;

        if (limits.stop != nullptr &&
            limits.stop->load(std::memory_order_relaxed)) {
            stopped = true;
            hard_stop = true;
            return true;
        }

        if (limits.node_limit > 0 && global_nodes() >= limits.node_limit) {
            stopped = true;
            hard_stop = true;
            return true;
        }

        if (!limits.infinite &&
            limits.hard_time_ms > 0 &&
            elapsed_ms() >= limits.hard_time_ms) {
            stopped = true;
            hard_stop = true;
            return true;
        }

        return false;
    }

    inline void poll_limits() noexcept {
        if ((nodes & 1023ULL) == 0)
            (void)hit_hard_limit();
    }

    [[nodiscard]] inline bool stop_after_completed_depth() noexcept {
        if (hit_hard_limit())
            return true;

        if (!limits.infinite &&
            limits.soft_time_ms > 0 &&
            elapsed_ms() >= limits.soft_time_ms) {
            stopped = true;
            return true;
        }

        return false;
    }

    [[nodiscard]] bool in_check(const Position& pos) const noexcept {
        const Color side = static_cast<Color>(pos.side_to_move);
        return checkers_bb(pos, mem, side) != 0ULL;
    }

    [[nodiscard]] inline bool use_nnue() const noexcept {
        return limits.use_nnue && nnue::loaded();
    }

    [[nodiscard]] inline int evaluate_position(const Position& pos) const noexcept {
        if (use_nnue())
            return nnue::to_cp(nnue::eval(pos), pos);
        return eval::evaluate(pos);
    }

    [[nodiscard]] bool is_threefold_repetition(
        const Position& pos,
        int ply
    ) const noexcept {
        int matches = 0;
        const int back = std::min(ply, pos.halfmove_clock);
        const int min_ply = ply - back;

        for (int p = ply - 2; p >= min_ply; p -= 2) {
            if (rep_keys[p] != pos.key)
                continue;
            if (++matches >= 2)
                return true;
        }

        return false;
    }

    [[nodiscard]] inline int repetition_score(const Position& pos) const noexcept {
        const int static_eval = evaluate_position(pos);
        return static_eval > 0 ? REPETITION_AVOID_SCORE : 0;
    }

    [[nodiscard]] bool see_ge(
        const Position& pos,
        Move move,
        int threshold
    ) const noexcept {
        if (!move_is_capture(move))
            return threshold <= 0;

        const Color us = static_cast<Color>(pos.side_to_move);
        const Square from = from_sq(move);
        const Square to = to_sq(move);
        const PieceType moving = piece_type_on(pos, from);
        if (!is_ok(moving))
            return false;

        const PieceType captured = move_is_ep(move)
            ? PAWN
            : piece_type_on(pos, to);
        if (!is_ok(captured))
            return false;

        int balance = see_piece_value[captured] - threshold;
        PieceType next_victim = moving;

        if (move_is_promotion(move)) {
            const PieceType promo = promo_piece(move);
            if (!is_ok(promo))
                return false;
            balance += see_piece_value[promo] - see_piece_value[PAWN];
            next_victim = promo;
        }

        if (balance < 0)
            return false;

        balance -= see_piece_value[next_victim];
        if (balance >= 0)
            return true;

        Bitboard occupied = pos.occupied ^ bb_of(from);
        if (move_is_ep(move)) {
            const Square cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
            occupied ^= bb_of(cap_sq);
        } else {
            occupied ^= bb_of(to);
        }

        const Bitboard bishop_like = pos.piece_bb[BISHOP] | pos.piece_bb[QUEEN];
        const Bitboard rook_like = pos.piece_bb[ROOK] | pos.piece_bb[QUEEN];
        Bitboard attackers = attackers_to(pos, mem, to, occupied);

        Color side = static_cast<Color>(us ^ 1);
        while (true) {
            attackers &= occupied;
            const Bitboard side_attackers = attackers & pos.color_bb[side];
            if (side_attackers == 0ULL)
                break;

            PieceType attacker = PIECE_TYPE_NONE;
            Bitboard from_set = 0ULL;

            Bitboard by_pt = side_attackers & pos.piece_bb[PAWN];
            if (by_pt) {
                attacker = PAWN;
                from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
            } else {
                by_pt = side_attackers & pos.piece_bb[KNIGHT];
                if (by_pt) {
                    attacker = KNIGHT;
                    from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                } else {
                    by_pt = side_attackers & pos.piece_bb[BISHOP];
                    if (by_pt) {
                        attacker = BISHOP;
                        from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                    } else {
                        by_pt = side_attackers & pos.piece_bb[ROOK];
                        if (by_pt) {
                            attacker = ROOK;
                            from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                        } else {
                            by_pt = side_attackers & pos.piece_bb[QUEEN];
                            if (by_pt) {
                                attacker = QUEEN;
                                from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                            } else {
                                by_pt = side_attackers & pos.piece_bb[KING];
                                if (by_pt) {
                                    attacker = KING;
                                    from_set = bb_of(static_cast<Square>(std::countr_zero(by_pt)));
                                }
                            }
                        }
                    }
                }
            }

            if (attacker == PIECE_TYPE_NONE)
                break;

            occupied ^= from_set;

            if (attacker == PAWN || attacker == BISHOP || attacker == QUEEN)
                attackers |= bishop_attacks(mem, to, occupied) & bishop_like;
            if (attacker == ROOK || attacker == QUEEN)
                attackers |= rook_attacks(mem, to, occupied) & rook_like;

            attackers &= occupied;
            balance = see_piece_value[attacker] - balance;
            side = static_cast<Color>(side ^ 1);

            if (balance >= 0) {
                if (attacker == KING && (attackers & pos.color_bb[side]) != 0ULL)
                    side = static_cast<Color>(side ^ 1);
                break;
            }
        }

        return side != us;
    }

    [[nodiscard]] bool has_non_pawn_material(
        const Position& pos,
        Color side
    ) const noexcept {
        return pieces(pos, side, KNIGHT) != 0ULL ||
               pieces(pos, side, BISHOP) != 0ULL ||
               pieces(pos, side, ROOK)   != 0ULL ||
               pieces(pos, side, QUEEN)  != 0ULL;
    }

    inline void do_null_move(Position& pos) const noexcept {
        // A null move simply passes the turn, clears ep state, and updates the
        // key. It is used for null-move pruning only; no board pieces move.
        if (has_ep(pos))
            pos.key ^= mem.tables.zobrist.ep_file[file_of(pos.ep_sq)];

        if (pos.side_to_move == BLACK)
            ++pos.fullmove_number;

        ++pos.halfmove_clock;
        pos.ep_sq = NO_SQ;
        pos.side_to_move ^= 1;
        pos.key ^= mem.tables.zobrist.side;
    }

    [[nodiscard]] inline Move tt_move_from_probe(
        const memory::TTProbe& probe
    ) const noexcept {
        return probe.hit ? static_cast<Move>(probe.data.move) : Move(0);
    }

    [[nodiscard]] inline bool tt_cutoff(
        const memory::TTProbe& probe,
        int depth,
        int alpha,
        int beta,
        int ply,
        int& score
    ) const noexcept {
        if (!probe.hit || probe.data.depth < depth)
            return false;

        score = score_from_tt(probe.data.score, ply);

        switch (static_cast<memory::Bound>(probe.data.flags & 0x3U)) {
            case memory::BOUND_EXACT:
                return true;
            case memory::BOUND_LOWER:
                return score >= beta;
            case memory::BOUND_UPPER:
                return score <= alpha;
            default:
                return false;
        }
    }

    inline void save_tt(
        const Position& pos,
        int depth,
        int ply,
        int score,
        int static_eval,
        Move best_move,
        int alpha,
        int beta,
        bool pv_node
    ) noexcept {
        memory::tt_save(
            mem.tt,
            pos.key,
            best_move,
            static_cast<i16>(score_to_tt(score, ply)),
            static_cast<i16>(static_eval),
            static_cast<i16>(depth),
            bound_from_score(score, alpha, beta),
            pv_node
        );
    }

    // PV lines are copied upward every time a child improves alpha.
    inline void update_pv(int ply, Move move) noexcept {
        pv_table[ply][0] = move;
        const int child_len = pv_length[ply + 1];
        if (child_len > 0) {
            std::memcpy(
                &pv_table[ply][1],
                pv_table[ply + 1],
                static_cast<std::size_t>(child_len) * sizeof(Move)
            );
        }
        pv_length[ply] = child_len + 1;
    }

    inline void bonus_history(const Position& pos, Move move, int depth) noexcept {
        // History only tracks quiet moves; captures are already ordered by MVV-LVA.
        if (move_is_capture(move))
            return;

        const Color side = static_cast<Color>(pos.side_to_move);
        const PieceType pt = piece_type_on(pos, from_sq(move));
        if (!is_ok(pt))
            return;

        i32& h = history[side][pt][to_sq(move)];
        h += depth * depth;
        if (h > 32767)
            h = 32767;
    }

    inline void penalty_history(const Position& pos, Move move, int depth) noexcept {
        if (move_is_capture(move))
            return;

        const Color side = static_cast<Color>(pos.side_to_move);
        const PieceType pt = piece_type_on(pos, from_sq(move));
        if (!is_ok(pt))
            return;

        i32& h = history[side][pt][to_sq(move)];
        h -= depth * depth * 4;
        if (h < -32767)
            h = -32767;
    }

    inline void penalize_quiets(
        const Position& pos,
        const Move* quiets,
        int count,
        Move excluded_move,
        int depth
    ) noexcept {
        for (int i = 0; i < count; ++i)
            if (quiets[i] != excluded_move)
                penalty_history(pos, quiets[i], depth);
    }

    inline void reward_history(const Position& pos, Move move, int depth, int ply) noexcept {
        // A quiet beta cutoff is a classic killer/history success signal.
        if (move_is_capture(move))
            return;

        if (killers[ply][0] != move) {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = move;
        }

        bonus_history(pos, move, depth);
    }

    [[nodiscard]] inline i32 score_move(
        const Position& pos,
        Move move,
        Move tt_move,
        int ply
    ) const noexcept {
        // Ordering priority:
        // 1. TT move
        // 2. captures by MVV-LVA
        // 3. promotions
        // 4. killer moves
        // 5. history heuristic
        if (move == tt_move)
            return 30'000'000;

        if (move_is_capture(move)) {
            const PieceType attacker = piece_type_on(pos, from_sq(move));
            const PieceType victim = move_is_ep(move) ? PAWN : piece_type_on(pos, to_sq(move));
            const int attacker_value = is_ok(attacker) ? piece_order_value[attacker] : 0;
            const int victim_value = is_ok(victim) ? piece_order_value[victim] : 0;
            return 20'000'000 + victim_value * 32 - attacker_value;
        }

        if (move_is_promotion(move))
            return 19'000'000 + piece_order_value[promo_piece(move)];

        if (move == killers[ply][0])
            return 18'000'000;
        if (move == killers[ply][1])
            return 17'999'000;

        const Color side = static_cast<Color>(pos.side_to_move);
        const PieceType pt = piece_type_on(pos, from_sq(move));
        return is_ok(pt) ? history[side][pt][to_sq(move)] : 0;
    }

    inline void score_moves(
        const Position& pos,
        const MoveList& moves,
        ScoredMoveList& scored,
        Move tt_move,
        int ply
    ) const noexcept {
        scored.size = moves.size;
        for (int i = 0; i < moves.size; ++i) {
            scored.moves[i].move = moves.moves[i];
            scored.moves[i].score = score_move(pos, moves.moves[i], tt_move, ply);
        }
    }

    [[nodiscard]] inline Move pick_next(ScoredMoveList& scored, int index) const noexcept {
        int best = index;
        for (int i = index + 1; i < scored.size; ++i)
            if (scored.moves[i].score > scored.moves[best].score)
                best = i;

        if (best != index)
            std::swap(scored.moves[index], scored.moves[best]);

        return scored.moves[index].move;
    }

    [[nodiscard]] int qsearch(Position& pos, int alpha, int beta, int ply) noexcept {
        // Quiescence search extends only tactical continuations (or all legal
        // evasions when in check) so the engine does not stand pat in unstable positions.
        pv_length[ply] = 0;
        rep_keys[ply] = pos.key;
        ++nodes;
        poll_limits();
        if (stopped)
            return alpha;

        if (ply >= MAX_PLY - 1)
            return evaluate_position(pos);

        if (pos.halfmove_clock >= 100)
            return 0;
        if (is_threefold_repetition(pos, ply))
            return repetition_score(pos);

        alpha = std::max(alpha, -VALUE_MATE + ply);
        beta = std::min(beta, VALUE_MATE - ply - 1);
        if (alpha >= beta)
            return alpha;

        const int alpha0 = alpha;
        const bool pv_node = (beta - alpha) > 1;
        const memory::TTProbe probe = memory::tt_probe(mem.tt, pos.key);

        int tt_score = 0;
        if (tt_cutoff(probe, 0, alpha, beta, ply, tt_score))
            return tt_score;

        const bool checked = in_check(pos);
        const Move tt_move = tt_move_from_probe(probe);
        const int static_eval = probe.hit ? probe.data.eval : evaluate_position(pos);

        if (!checked) {
            // Stand-pat: if the static position already fails high, no capture
            // search can make it worse for the side to move.
            if (static_eval >= beta) {
                save_tt(pos, 0, ply, static_eval, static_eval, tt_move, alpha0, beta, pv_node);
                return static_eval;
            }
            if (static_eval > alpha)
                alpha = static_eval;
        }

        MoveList list{};
        GenInfo info{};
        init_gen_info(info, pos, mem);
        Move* qend = generate_pseudo_captures(pos, mem, info, list.moves);
        list.size = static_cast<int>(qend - list.moves);

        if (list.size == 0) {
            const int score = checked ? (-VALUE_MATE + ply) : alpha;
            save_tt(pos, 0, ply, score, static_eval, 0, alpha0, beta, pv_node);
            return score;
        }

        ScoredMoveList scored{};
        score_moves(pos, list, scored, tt_move, ply);

        Move best_move = 0;
        int legal_count = 0;
        for (int i = 0; i < scored.size; ++i) {
            const Move move = pick_next(scored, i);
            if (!legal_fast(pos, mem, info, move))
                continue;

            ++legal_count;

            if (!checked &&
                !move_is_promotion(move) &&
                !see_ge(pos, move, -DELTA_MARGIN)) {
                continue;
            }

            if (!checked && !move_is_promotion(move)) {
                // Delta pruning skips captures that cannot reasonably raise alpha.
                const int max_gain = capture_gain_estimate(pos, move);
                if (static_eval + max_gain + DELTA_MARGIN <= alpha)
                    continue;
            }

            StateInfo st{};
            make_move(pos, move, mem.tables, st);
            memory::tt_prefetch(mem.tt, pos.key);

            const int score = -qsearch(pos, -beta, -alpha, ply + 1);
            unmake_move(pos, move, mem.tables, st);
            if (score > alpha) {
                alpha = score;
                best_move = move;
                update_pv(ply, move);
                if (alpha >= beta)
                    break;
            }
        }

        if (legal_count == 0) {
            const int score = checked ? (-VALUE_MATE + ply) : alpha;
            save_tt(pos, 0, ply, score, static_eval, 0, alpha0, beta, pv_node);
            return score;
        }

        save_tt(pos, 0, ply, alpha, static_eval, best_move, alpha0, beta, pv_node);
        return alpha;
    }

    [[nodiscard]] int pvs(
        Position& pos,
        int depth,
        int alpha,
        int beta,
        int ply,
        bool allow_null
    ) noexcept {
        // Principal Variation Search:
        // - first move gets a full window
        // - later moves get a null window first
        // - promising moves are re-searched on a wider window
        pv_length[ply] = 0;
        rep_keys[ply] = pos.key;

        if (stopped)
            return alpha;

        if (ply >= MAX_PLY - 1)
            return evaluate_position(pos);

        if (pos.halfmove_clock >= 100)
            return 0;
        if (is_threefold_repetition(pos, ply))
            return repetition_score(pos);

        alpha = std::max(alpha, -VALUE_MATE + ply);
        beta = std::min(beta, VALUE_MATE - ply - 1);
        if (alpha >= beta)
            return alpha;

        if (depth <= 0)
            return qsearch(pos, alpha, beta, ply);

        ++nodes;
        poll_limits();
        if (stopped)
            return alpha;

        const int alpha0 = alpha;
        const bool pv_node = (beta - alpha) > 1;
        const memory::TTProbe probe = memory::tt_probe(mem.tt, pos.key);
        const Move tt_move = tt_move_from_probe(probe);

        int search_depth = depth;
        if (!pv_node &&
            search_depth >= IIR_MIN_DEPTH &&
            move_is_none(tt_move)) {
            --search_depth;
        }

        int tt_score = 0;
        if (tt_cutoff(probe, search_depth, alpha, beta, ply, tt_score))
            return tt_score;

        const int static_eval = probe.hit ? probe.data.eval : evaluate_position(pos);
        const bool checked = in_check(pos);
        const Color side = static_cast<Color>(pos.side_to_move);

        if (!pv_node &&
            !checked &&
            search_depth <= 2 &&
            static_eval + RAZOR_MARGIN[search_depth] <= alpha) {
            // Razoring: at very shallow depth, a bad static eval can defer to qsearch.
            const int score = qsearch(pos, alpha, beta, ply);
            if (score <= alpha)
                return score;
        }

        if (!pv_node &&
            !checked &&
            search_depth <= 3 &&
            !is_mate_window(beta) &&
            static_eval - REVERSE_FUTILITY_MARGIN[search_depth] >= beta) {
            // Reverse futility: when static eval is already safely above beta,
            // a shallow node often does not need a full search.
            return static_eval;
        }

        if (allow_null &&
            !pv_node &&
            !checked &&
            search_depth >= 3 &&
            !is_mate_window(beta) &&
            static_eval >= beta &&
            has_non_pawn_material(pos, side)) {
            // Null-move pruning tests whether simply passing still keeps the
            // position above beta. If so, the real position is likely also a cutoff.
            Position null_pos = pos;
            do_null_move(null_pos);

            const int reduction = null_reduction(search_depth);
            const int score = -pvs(
                null_pos,
                search_depth - 1 - reduction,
                -beta,
                -beta + 1,
                ply + 1,
                false
            );

            if (score >= beta) {
                save_tt(pos, search_depth, ply, score, static_eval, 0, alpha0, beta, pv_node);
                return score;
            }
        }

        MoveList list{};
        GenInfo info{};
        init_gen_info(info, pos, mem);
        Move* pend = generate_pseudo_legal(pos, mem, info, list.moves);
        list.size = static_cast<int>(pend - list.moves);

        ScoredMoveList scored{};
        score_moves(pos, list, scored, tt_move, ply);

        bool searched_first = false;
        Move best_move = 0;
        int legal_count = 0;
        int quiet_count = 0;
        Move searched_quiets[MAX_MOVES];
        int searched_quiet_count = 0;
        bool cutoff = false;

        for (int i = 0; i < scored.size; ++i) {
            if (stopped)
                break;

            const Move move = pick_next(scored, i);
            if (!legal_fast(pos, mem, info, move))
                continue;

            ++legal_count;
            const bool quiet_move =
                !move_is_capture(move) &&
                !move_is_promotion(move) &&
                !move_is_castle(move);
            const bool simple_capture =
                move_is_capture(move) &&
                !move_is_promotion(move);
            const int history_score = quiet_move
                ? history[side][piece_type_on(pos, from_sq(move))][to_sq(move)]
                : 0;

            if (quiet_move)
                ++quiet_count;

            if (!pv_node &&
                !checked &&
                search_depth <= SEE_PRUNE_DEPTH_LIMIT &&
                simple_capture &&
                i > 1 &&
                !see_ge(pos, move, -60 * search_depth)) {
                // Bad capture pruning: skip late captures that fail a SEE threshold.
                continue;
            }

            if (!pv_node &&
                !checked &&
                search_depth <= 2 &&
                quiet_move &&
                static_eval + FUTILITY_MARGIN[search_depth] <= alpha) {
                // Shallow futility pruning skips quiet moves that cannot raise alpha.
                continue;
            }

            if (!pv_node &&
                !checked &&
                search_depth <= 4 &&
                quiet_move &&
                quiet_count > lmp_limit(search_depth) &&
                static_eval <= alpha) {
                // Late move pruning drops very late quiets once enough earlier
                // quiets have already been searched with no improvement.
                continue;
            }

            if (!pv_node &&
                !checked &&
                search_depth <= 4 &&
                quiet_move &&
                quiet_count > lmp_limit(search_depth) / 2 &&
                history_score <= history_prune_threshold(search_depth)) {
                // History pruning removes quiets that are both late and historically bad.
                continue;
            }

            StateInfo st{};
            make_move(pos, move, mem.tables, st);
            memory::tt_prefetch(mem.tt, pos.key);

            int score = 0;
            if (!searched_first) {
                score = -pvs(pos, search_depth - 1, -beta, -alpha, ply + 1, true);
                searched_first = true;
            } else {
                if (!pv_node &&
                    !checked &&
                    quiet_move &&
                    search_depth >= 3 &&
                    i >= 3) {
                    // Late Move Reductions search late quiets at a reduced depth first.
                    const int reduction = lmr_reduction(search_depth, i);
                    score = -pvs(
                        pos,
                        search_depth - 1 - reduction,
                        -alpha - 1,
                        -alpha,
                        ply + 1,
                        true
                    );

                    if (score > alpha)
                        // If the reduced search still looks interesting, restore
                        // the original depth and test it again on a narrow window.
                        score = -pvs(pos, search_depth - 1, -alpha - 1, -alpha, ply + 1, true);
                } else {
                    score = -pvs(pos, search_depth - 1, -alpha - 1, -alpha, ply + 1, true);
                }

                if (score > alpha && score < beta)
                    // A null-window fail-high inside the PV must be confirmed by
                    // a full-window re-search before the score is trusted.
                    score = -pvs(pos, search_depth - 1, -beta, -alpha, ply + 1, true);
            }

            unmake_move(pos, move, mem.tables, st);

            if (quiet_move)
                searched_quiets[searched_quiet_count++] = move;

            if (score > alpha) {
                alpha = score;
                best_move = move;
                update_pv(ply, move);
                if (alpha >= beta) {
                    reward_history(pos, move, depth, ply);
                    penalize_quiets(pos, searched_quiets, searched_quiet_count, move, depth);
                    cutoff = true;
                    break;
                }
            }
        }

        if (legal_count == 0) {
            const int score = checked ? (-VALUE_MATE + ply) : 0;
            save_tt(pos, search_depth, ply, score, static_eval, 0, alpha0, beta, pv_node);
            return score;
        }

        if (!cutoff &&
            alpha > alpha0 &&
            best_move != 0 &&
            !move_is_capture(best_move)) {
            bonus_history(pos, best_move, std::max(1, search_depth - 1));
            penalize_quiets(pos, searched_quiets, searched_quiet_count, best_move, std::max(1, search_depth - 1));
        }

        save_tt(pos, search_depth, ply, alpha, static_eval, best_move, alpha0, beta, pv_node);
        return alpha;
    }

    [[nodiscard]] SearchResult search_root(
        Position root,
        int depth,
        Move hint_move,
        int alpha,
        int beta
    ) noexcept {
        // Root search mirrors PVS, but it also keeps the best move/result for UCI output.
        SearchResult result{};
        result.depth = depth;
        rep_keys[0] = root.key;

        MoveList list{};
        GenInfo info{};
        init_gen_info(info, root, mem);
        Move* rend = generate_pseudo_legal(root, mem, info, list.moves);
        list.size = static_cast<int>(rend - list.moves);

        if (limits.root_move_count > 0) {
            MoveList filtered{};
            for (int i = 0; i < list.size; ++i) {
                const Move move = list.moves[i];
                bool allowed = false;
                for (int j = 0; j < limits.root_move_count; ++j) {
                    if (limits.root_moves[j] == move) {
                        allowed = true;
                        break;
                    }
                }
                if (allowed)
                    filtered.moves[filtered.size++] = move;
            }
            list = filtered;
        }

        const memory::TTProbe probe = memory::tt_probe(mem.tt, root.key);
        const Move tt_move = tt_move_from_probe(probe);
        const Move root_hint = move_is_none(tt_move) ? hint_move : tt_move;
        const int static_eval = probe.hit ? probe.data.eval : evaluate_position(root);
        const int alpha0 = alpha;
        const bool checked = in_check(root);

        ScoredMoveList scored{};
        score_moves(root, list, scored, root_hint, 0);
        int best_score = -VALUE_INF;
        int legal_count = 0;
        result.best_move = 0;

        for (int i = 0; i < scored.size; ++i) {
            if (hit_hard_limit())
                break;

            const Move move = pick_next(scored, i);
            if (!legal_fast(root, mem, info, move))
                continue;

            ++legal_count;
            StateInfo st{};
            make_move(root, move, mem.tables, st);
            memory::tt_prefetch(mem.tt, root.key);

            int score = 0;
            if (i == 0) {
                score = -pvs(root, depth - 1, -beta, -alpha, 1, true);
            } else {
                score = -pvs(root, depth - 1, -alpha - 1, -alpha, 1, true);
                if (score > alpha)
                    score = -pvs(root, depth - 1, -beta, -alpha, 1, true);
            }

            unmake_move(root, move, mem.tables, st);

            if (score > best_score) {
                best_score = score;
                result.best_move = move;
            }

            if (score > alpha) {
                alpha = score;
                update_pv(0, move);
                if (alpha >= beta)
                    break;
            }
        }

        if (legal_count == 0) {
            result.score = checked ? -VALUE_MATE : 0;
            result.best_move = 0;
            return result;
        }

        if (best_score == -VALUE_INF)
            best_score = alpha;

        result.score = best_score;
        result.nodes = nodes;
        save_tt(root, depth, 0, best_score, static_eval, result.best_move, alpha0, beta, true);
        return result;
    }
};

} // namespace

std::string move_to_uci(Move m) {
    if (move_is_none(m))
        return "0000";

    std::string s;
    s.reserve(5);
    s.push_back(static_cast<char>('a' + file_of(from_sq(m))));
    s.push_back(static_cast<char>('1' + rank_of(from_sq(m))));
    s.push_back(static_cast<char>('a' + file_of(to_sq(m))));
    s.push_back(static_cast<char>('1' + rank_of(to_sq(m))));

    if (move_is_promotion(m)) {
        switch (promo_piece(m)) {
            case KNIGHT: s.push_back('n'); break;
            case BISHOP: s.push_back('b'); break;
            case ROOK:   s.push_back('r'); break;
            case QUEEN:  s.push_back('q'); break;
            default: break;
        }
    }

    return s;
}

SearchResult iterative_deepening(
    const Position& root,
    memory::Memory& mem,
    const SearchLimits& limits,
    std::ostream* out
) {
    // Iterative deepening provides progressively better moves, while aspiration
    // windows reuse the previous iteration score to narrow the root window.
    memory::memory_new_search(mem);

    Searcher searcher(mem, limits);
    SearchResult best{};
    Move hint_move = 0;
    Position keyed_root = root;
    position_refresh_key(keyed_root, mem.tables);
    const auto search_start = Searcher::clock::now();
    searcher.start_time = search_start;
    u64 total_nodes = 0;

    const int max_depth = std::max(1, limits.depth);

    for (int depth = 1; depth <= max_depth; ++depth) {
        SearchResult current{};
        u64 depth_nodes = 0;

        int alpha = -VALUE_INF;
        int beta = VALUE_INF;
        int delta = ASPIRATION_DELTA;

        if (depth >= 2) {
            alpha = std::max(-VALUE_INF, best.score - delta);
            beta = std::min(VALUE_INF, best.score + delta);
        }

        while (true) {
            if (searcher.stopped)
                break;

            searcher.nodes = 0;
            searcher.base_nodes = total_nodes + depth_nodes;
            std::fill(std::begin(searcher.pv_length), std::end(searcher.pv_length), 0);

            current = searcher.search_root(keyed_root, depth, hint_move, alpha, beta);
            depth_nodes += current.nodes;

            if (searcher.stopped || depth == 1)
                break;

            if (current.score <= alpha) {
                alpha = std::max(-VALUE_INF, current.score - delta);
                beta = std::min(VALUE_INF, current.score + delta);
                delta *= 2;
                continue;
            }

            if (current.score >= beta) {
                alpha = std::max(-VALUE_INF, current.score - delta);
                beta = std::min(VALUE_INF, current.score + delta);
                delta *= 2;
                continue;
            }

            break;
        }

        const auto end = Searcher::clock::now();
        total_nodes += depth_nodes;
        const bool stopped_mid_depth = searcher.stopped && best.depth > 0;

        if (!searcher.stopped || best.depth == 0) {
            best = current;
            best.nodes = total_nodes;
            hint_move = current.best_move;
        }

        if (out && !stopped_mid_depth) {
            const double seconds =
                std::chrono::duration<double>(end - search_start).count();
            const u64 nps = seconds > 0.0
                ? static_cast<u64>(static_cast<double>(total_nodes) / seconds)
                : 0ULL;

            *out << "info depth " << depth
                 << " score cp " << current.score
                 << " nodes " << total_nodes
                 << " time " << static_cast<u64>(seconds * 1000.0)
                 << " nps " << nps
                 << " hashfull " << memory::tt_hashfull(mem.tt)
                 << " pv";

            for (int i = 0; i < searcher.pv_length[0]; ++i)
                *out << ' ' << move_to_uci(searcher.pv_table[0][i]);

            *out << '\n';
        }

        if (stopped_mid_depth)
            break;

        if (searcher.stop_after_completed_depth())
            break;
    }

    best.nodes = total_nodes;
    return best;
}

} // namespace valerain::search
