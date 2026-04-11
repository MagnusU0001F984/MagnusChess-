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

#include "Evaluation.h"
#include "History.h"
#include "Lmr.h"
#include "MovePicker.h"
#include "MoveGen.h"
#include "Nmp.h"
#include "Nnue.h"
#include "See.h"

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
constexpr int VALUE_NONE = 32002;
constexpr int DELTA_MARGIN = 200;
constexpr int QS_ADJ_SHUFFLE_CAP = 80;
constexpr int FUTILITY_BASE_MARGIN = 72;
constexpr int FUTILITY_DEPTH_MARGIN = 72;
constexpr int FUTILITY_IMPROVING_MARGIN = 24;
constexpr int FUTILITY_HISTORY_DIVISOR = 128;
constexpr int RFP_BASE_MARGIN = 56;
constexpr int RFP_DEPTH_MARGIN = 72;
constexpr int RFP_IMPROVING_MARGIN = 40;
constexpr int RFP_CORRECTION_THRESHOLD = 128;
constexpr int RFP_CORRECTION_MARGIN_BONUS = 8;
constexpr int RFP_TT_CAPTURE_MARGIN_REDUCTION = 16;
constexpr int RFP_TT_QUIET_FAIL_HIGH_BONUS = 24;
constexpr int RAZOR_MARGIN[3] = { 0, 280, 420 };
constexpr int ASPIRATION_DELTA = 24;
constexpr int REPETITION_AVOID_SCORE = -16;
constexpr int IIR_MIN_DEPTH = 6;
constexpr int SEE_PRUNE_DEPTH_LIMIT = 6;
constexpr int IMPROVING_MARGIN = 16;
constexpr int CAPTURE_HISTORY_HIGH_THRESHOLD = 128;
constexpr int CAPTURE_TOPK = 3;
constexpr int PROBCUT_MIN_DEPTH = 5;
constexpr int PROBCUT_MARGIN = 224;
constexpr int PROBCUT_REDUCTION = 5;
constexpr int PROBCUT_TT_DEPTH_MARGIN = 3;
constexpr int SINGULAR_MIN_DEPTH = 8;
constexpr int SINGULAR_TT_DEPTH_MARGIN = 3;
constexpr int SINGULAR_MARGIN_BASE = 24;
constexpr int SINGULAR_MARGIN_PER_DEPTH = 4;
constexpr int SINGULAR_DOUBLE_MARGIN_BASE = 48;
constexpr int SINGULAR_DOUBLE_MARGIN_PER_DEPTH = 8;
constexpr int SINGULAR_DOUBLE_MIN_DEPTH = 12;
constexpr int SEE_LATE_BAD_CAPTURE_GATE_MIN_DEPTH = 4;
constexpr int SEE_LATE_BAD_CAPTURE_GATE_MAX_DEPTH = 8;
constexpr int SEE_LATE_BAD_CAPTURE_GATE_MIN_CAPTURE_INDEX = 4;
constexpr int HASHFULL_REPORT_PERIOD = 4;
constexpr int CORRECTION_HISTORY_SIZE = 16384;
constexpr int CORRECTION_HISTORY_GRAIN = 16;
constexpr int CORRECTION_HISTORY_CLAMP = 256;
constexpr int CORRECTION_HISTORY_WEIGHT_MAX = 96;
constexpr int CORRECTION_POSITION_WEIGHT = 2;
constexpr int CORRECTION_PAWN_WEIGHT = 2;
constexpr int CORRECTION_MATERIAL_WEIGHT = 1;
constexpr int CORRECTION_WEIGHT_SUM =
    CORRECTION_POSITION_WEIGHT
    + CORRECTION_PAWN_WEIGHT
    + CORRECTION_MATERIAL_WEIGHT;

#ifndef VALERAIN_SEARCH_OBS
#define VALERAIN_SEARCH_OBS 0
#endif

#ifndef VALERAIN_ENABLE_PROBCUT
#define VALERAIN_ENABLE_PROBCUT 1
#endif

#ifndef VALERAIN_ENABLE_SINGULAR_EXTENSION
#define VALERAIN_ENABLE_SINGULAR_EXTENSION 1
#endif

#ifndef VALERAIN_SEE_LATE_BAD_CAPTURE_GATE_THRESHOLD
#define VALERAIN_SEE_LATE_BAD_CAPTURE_GATE_THRESHOLD -60
#endif
constexpr int SEE_LATE_BAD_CAPTURE_GATE_THRESHOLD =
    VALERAIN_SEE_LATE_BAD_CAPTURE_GATE_THRESHOLD;

#ifndef VALERAIN_SEE_TERM_PRESET
#define VALERAIN_SEE_TERM_PRESET 1
#endif

#if VALERAIN_SEE_TERM_PRESET == 0
constexpr SeeScalePreset SEE_TERM_PRESET = SeeScalePreset::Weak;
#elif VALERAIN_SEE_TERM_PRESET == 1
constexpr SeeScalePreset SEE_TERM_PRESET = SeeScalePreset::Medium;
#elif VALERAIN_SEE_TERM_PRESET == 2
constexpr SeeScalePreset SEE_TERM_PRESET = SeeScalePreset::Strong;
#else
#error "VALERAIN_SEE_TERM_PRESET must be 0 (Weak), 1 (Medium), or 2 (Strong)"
#endif

#ifndef VALERAIN_CAPTURE_OBS
// #define VALERAIN_CAPTURE_OBS 1
#define VALERAIN_CAPTURE_OBS 0
#endif

#ifndef VALERAIN_SEE_LATE_BAD_CAPTURE_GATE
#define VALERAIN_SEE_LATE_BAD_CAPTURE_GATE 1
#endif

#ifndef VALERAIN_MOVEPICKER_OBS
#define VALERAIN_MOVEPICKER_OBS 1
#endif

constexpr int piece_order_value[PIECE_TYPE_NB] = {
    100, 320, 330, 500, 900, 0
};

[[nodiscard]] static inline int mvv_lva_capture_term(
    const Position& pos,
    Move move
) noexcept {
    const PieceType attacker = piece_type_on(pos, from_sq(move));
    const PieceType victim = move_is_ep(move) ? PAWN : piece_type_on(pos, to_sq(move));
    const int attacker_value = is_ok(attacker) ? piece_order_value[attacker] : 0;
    const int victim_value = is_ok(victim) ? piece_order_value[victim] : 0;
    return victim_value * 32 - attacker_value;
}

inline void append_uci_score(
    std::ostream& out,
    int score,
    const Position& root,
    bool nnue_winrate_score
) {
    if (score >= VALUE_MATE - MAX_PLY) {
        const int plies_to_mate = VALUE_MATE - score;
        out << "score mate " << ((plies_to_mate + 1) / 2);
        return;
    }

    if (score <= -VALUE_MATE + MAX_PLY) {
        const int plies_to_mate = VALUE_MATE + score;
        out << "score mate -" << ((plies_to_mate + 1) / 2);
        return;
    }

    const int display_score = nnue_winrate_score
        ? nnue::search_score_to_cp(score, root)
        : score;
    out << "score cp " << display_score;

    if (nnue_winrate_score) {
        const nnue::WdlTriplet wdl = nnue::search_score_to_wdl(score, root);
        out << " wdl " << wdl.win << ' ' << wdl.draw << ' ' << wdl.loss;
    }
}

/*
Searcher owns the mutable state for one iterative-deepening session: node
counting, killer/history tables, PV storage, and stop-condition bookkeeping.
*/
struct Searcher {
    struct CorrectionKeys {
        Key position = 0;
        Key pawn_king = 0;
        Key material = 0;
    };

    struct StaticEvalInfo {
        CorrectionKeys keys{};
        int raw = VALUE_NONE;
        int corrected = 0;
        int stand_pat = 0;
    };

    using clock = std::chrono::steady_clock;

    memory::Memory& mem;
    const SearchLimits& limits;

    u64 nodes = 0;
    u64 base_nodes = 0;
    int seldepth = 0;
    HistoryTables history_tables{};
    Move pv_table[MAX_PLY][MAX_PLY]{};
    int pv_length[MAX_PLY + 1]{};
    Key rep_keys[MAX_PLY + 1]{};
    Move move_stack[MAX_PLY]{};
    SearchStackEntry search_stack[MAX_PLY + 2]{};
    int static_eval_stack[MAX_PLY + 1]{};
    bool static_eval_valid[MAX_PLY + 1]{};
    i16 position_correction_history[COLOR_NB][CORRECTION_HISTORY_SIZE]{};
    i16 pawn_correction_history[COLOR_NB][CORRECTION_HISTORY_SIZE]{};
    i16 material_correction_history[COLOR_NB][CORRECTION_HISTORY_SIZE]{};
    clock::time_point start_time{};
    bool stopped = false;
    bool hard_stop = false;
    int nmp_min_ply = 0;

#if VALERAIN_CAPTURE_OBS
    struct CaptureObservation {
        u64 main_capture_searches = 0;
        u64 main_capture_cutoffs = 0;
        u64 q_capture_searches = 0;
        u64 q_capture_cutoffs = 0;

        u64 topk_tried[CAPTURE_TOPK]{};
        u64 topk_cutoff[CAPTURE_TOPK]{};

        u64 main_see_tried[3]{};
        u64 main_see_cutoff[3]{};
        u64 q_see_tried[3]{};
        u64 q_see_cutoff[3]{};

        u64 main_high_hist_tried = 0;
        u64 main_high_hist_cutoff = 0;
        u64 q_high_hist_tried = 0;
        u64 q_high_hist_cutoff = 0;

        i64 topk_rank_shift_sum[CAPTURE_TOPK]{};
        u64 topk_rank_shift_count[CAPTURE_TOPK]{};

        u64 gate_checks = 0;
        u64 gate_bad_see = 0;
        u64 gate_pruned = 0;
        u64 gate_exempt_check = 0;
        u64 gate_exempt_recapture = 0;

        u64 cap_lmr_late_simple_total = 0;
        u64 cap_lmr_eligible = 0;
        u64 cap_lmr_considered = 0;
        u64 cap_lmr_reduced = 0;
        u64 cap_lmr_reduced_r1 = 0;
        u64 cap_lmr_reduced_r2 = 0;
        u64 cap_lmr_research = 0;
        u64 cap_lmr_research_r1 = 0;
        u64 cap_lmr_research_r2 = 0;
        u64 cap_lmr_considered_see[3]{};
        u64 cap_lmr_reduced_see[3]{};
        u64 cap_lmr_research_see[3]{};
    } cap_obs{};
#endif

#if VALERAIN_MOVEPICKER_OBS
    enum class MoveStageBucket : std::uint8_t {
        TT = 0,
        GoodCapture,
        Killer,
        Quiet,
        BadCapture,
        Count
    };

    struct MovePickerObservation {
        u64 nodes = 0;
        u64 nodes_with_tt_probe = 0;
        u64 tt_first_try = 0;
        u64 tt_first_cutoff = 0;

        u64 cutoffs_total = 0;
        u64 cutoff_by_stage[static_cast<int>(MoveStageBucket::Count)]{};

        u64 first_good_capture_try = 0;
        u64 first_good_capture_cutoff = 0;
        u64 first_killer_try = 0;
        u64 first_killer_cutoff = 0;
        u64 first_quiet_try = 0;
        u64 first_quiet_cutoff = 0;

        u64 quiet_generated = 0;
        u64 quiet_scored = 0;
        u64 quiet_skipped_by_mp = 0;
        u64 quiet_searched = 0;
        u64 late_quiet_fail_high = 0;
        u64 quiet_fail_high_after_skip_band = 0;
    } mp_obs{};
#endif

#if VALERAIN_SEARCH_OBS
    struct SearchObservation {
        u64 nmp_candidates = 0;
        u64 nmp_tried = 0;
        u64 nmp_fail_high = 0;
        u64 nmp_verification_tried = 0;
        u64 nmp_verified_cutoffs = 0;
        u64 nmp_verification_failed = 0;
        u64 probcut_nodes = 0;
        u64 probcut_moves = 0;
        u64 probcut_cutoffs = 0;
        u64 singular_candidates = 0;
        u64 singular_extend1 = 0;
        u64 singular_extend2 = 0;
    } search_obs{};
#endif

    explicit Searcher(memory::Memory& m, const SearchLimits& l) noexcept
        : mem(m), limits(l) {}

#if VALERAIN_CAPTURE_OBS
    [[nodiscard]] static inline int see_bucket(int see_value) noexcept {
        if (see_value < 0) return 0;
        if (see_value == 0) return 1;
        return 2;
    }

    [[nodiscard]] static inline int ratio_percent(u64 num, u64 den) noexcept {
        if (den == 0) return 0;
        return static_cast<int>((num * 100ULL) / den);
    }

    [[nodiscard]] static inline int lookup_capture_base_rank(
        Move move,
        const Move* cap_moves,
        const int* cap_ranks,
        int cap_count
    ) noexcept {
        for (int i = 0; i < cap_count; ++i) {
            if (cap_moves[i] == move)
                return cap_ranks[i];
        }
        return 0;
    }

    inline void record_main_capture_try(
        int capture_order,
        int see_value,
        int capture_hist,
        int base_rank
    ) noexcept {
        ++cap_obs.main_capture_searches;
        const int bucket = see_bucket(see_value);
        ++cap_obs.main_see_tried[bucket];
        if (capture_hist >= CAPTURE_HISTORY_HIGH_THRESHOLD)
            ++cap_obs.main_high_hist_tried;

        if (capture_order >= 0 && capture_order < CAPTURE_TOPK) {
            ++cap_obs.topk_tried[capture_order];
            if (base_rank > 0) {
                cap_obs.topk_rank_shift_sum[capture_order] += static_cast<i64>(base_rank - (capture_order + 1));
                ++cap_obs.topk_rank_shift_count[capture_order];
            }
        }
    }

    inline void record_main_capture_cutoff(
        int capture_order,
        int see_value,
        int capture_hist
    ) noexcept {
        ++cap_obs.main_capture_cutoffs;
        const int bucket = see_bucket(see_value);
        ++cap_obs.main_see_cutoff[bucket];
        if (capture_hist >= CAPTURE_HISTORY_HIGH_THRESHOLD)
            ++cap_obs.main_high_hist_cutoff;
        if (capture_order >= 0 && capture_order < CAPTURE_TOPK)
            ++cap_obs.topk_cutoff[capture_order];
    }

    inline void record_q_capture_try(
        int see_value,
        int capture_hist
    ) noexcept {
        ++cap_obs.q_capture_searches;
        const int bucket = see_bucket(see_value);
        ++cap_obs.q_see_tried[bucket];
        if (capture_hist >= CAPTURE_HISTORY_HIGH_THRESHOLD)
            ++cap_obs.q_high_hist_tried;
    }

    inline void record_q_capture_cutoff(
        int see_value,
        int capture_hist
    ) noexcept {
        ++cap_obs.q_capture_cutoffs;
        const int bucket = see_bucket(see_value);
        ++cap_obs.q_see_cutoff[bucket];
        if (capture_hist >= CAPTURE_HISTORY_HIGH_THRESHOLD)
            ++cap_obs.q_high_hist_cutoff;
    }

    inline void record_gate_check(
        bool bad_see,
        bool pruned,
        bool exempt_check,
        bool exempt_recapture
    ) noexcept {
        ++cap_obs.gate_checks;
        if (bad_see)
            ++cap_obs.gate_bad_see;
        if (pruned)
            ++cap_obs.gate_pruned;
        if (exempt_check)
            ++cap_obs.gate_exempt_check;
        if (exempt_recapture)
            ++cap_obs.gate_exempt_recapture;
    }

    inline void record_cap_lmr_considered(
        int see_value,
        int reduction
    ) noexcept {
        ++cap_obs.cap_lmr_considered;
        const int bucket = see_bucket(see_value);
        ++cap_obs.cap_lmr_considered_see[bucket];

        if (reduction <= 0)
            return;

        ++cap_obs.cap_lmr_reduced;
        ++cap_obs.cap_lmr_reduced_see[bucket];
        if (reduction == 1)
            ++cap_obs.cap_lmr_reduced_r1;
        else
            ++cap_obs.cap_lmr_reduced_r2;
    }

    inline void record_cap_lmr_research(
        int see_value,
        int reduction
    ) noexcept {
        if (reduction <= 0)
            return;

        ++cap_obs.cap_lmr_research;
        const int bucket = see_bucket(see_value);
        ++cap_obs.cap_lmr_research_see[bucket];
        if (reduction == 1)
            ++cap_obs.cap_lmr_research_r1;
        else
            ++cap_obs.cap_lmr_research_r2;
    }

    inline void emit_capture_observation(std::ostream& out) const {
        const auto sb_raw = [&](DepthClass dc, SeeClass sc) -> int {
            return static_cast<int>(
                history_tables.see_bias.value[static_cast<int>(dc)][static_cast<int>(sc)]
            );
        };
        const auto sb_term = [](int raw) -> int {
            return std::clamp(raw / 4, -96, 96);
        };

        out << "info string capobs main_capture "
            << cap_obs.main_capture_cutoffs << '/' << cap_obs.main_capture_searches
            << " (" << ratio_percent(cap_obs.main_capture_cutoffs, cap_obs.main_capture_searches) << "%)"
            << " q_capture "
            << cap_obs.q_capture_cutoffs << '/' << cap_obs.q_capture_searches
            << " (" << ratio_percent(cap_obs.q_capture_cutoffs, cap_obs.q_capture_searches) << "%)\n";

        out << "info string capobs topk "
            << "k1 " << cap_obs.topk_cutoff[0] << '/' << cap_obs.topk_tried[0]
            << " (" << ratio_percent(cap_obs.topk_cutoff[0], cap_obs.topk_tried[0]) << "%) "
            << "k2 " << cap_obs.topk_cutoff[1] << '/' << cap_obs.topk_tried[1]
            << " (" << ratio_percent(cap_obs.topk_cutoff[1], cap_obs.topk_tried[1]) << "%) "
            << "k3 " << cap_obs.topk_cutoff[2] << '/' << cap_obs.topk_tried[2]
            << " (" << ratio_percent(cap_obs.topk_cutoff[2], cap_obs.topk_tried[2]) << "%)\n";

        out << "info string capobs see_cutoff_rate "
            << "main_bad " << cap_obs.main_see_cutoff[0] << '/' << cap_obs.main_see_tried[0]
            << " (" << ratio_percent(cap_obs.main_see_cutoff[0], cap_obs.main_see_tried[0]) << "%) "
            << "main_eq " << cap_obs.main_see_cutoff[1] << '/' << cap_obs.main_see_tried[1]
            << " (" << ratio_percent(cap_obs.main_see_cutoff[1], cap_obs.main_see_tried[1]) << "%) "
            << "main_good " << cap_obs.main_see_cutoff[2] << '/' << cap_obs.main_see_tried[2]
            << " (" << ratio_percent(cap_obs.main_see_cutoff[2], cap_obs.main_see_tried[2]) << "%) "
            << "q_bad " << cap_obs.q_see_cutoff[0] << '/' << cap_obs.q_see_tried[0]
            << " (" << ratio_percent(cap_obs.q_see_cutoff[0], cap_obs.q_see_tried[0]) << "%) "
            << "q_eq " << cap_obs.q_see_cutoff[1] << '/' << cap_obs.q_see_tried[1]
            << " (" << ratio_percent(cap_obs.q_see_cutoff[1], cap_obs.q_see_tried[1]) << "%) "
            << "q_good " << cap_obs.q_see_cutoff[2] << '/' << cap_obs.q_see_tried[2]
            << " (" << ratio_percent(cap_obs.q_see_cutoff[2], cap_obs.q_see_tried[2]) << "%)\n";

        out << "info string capobs gate "
            << "checked " << cap_obs.gate_checks
            << " bad_see " << cap_obs.gate_bad_see
            << " pruned " << cap_obs.gate_pruned
            << " (" << ratio_percent(cap_obs.gate_pruned, cap_obs.gate_checks) << "%)"
            << " exempt_check " << cap_obs.gate_exempt_check
            << " exempt_recapture " << cap_obs.gate_exempt_recapture << '\n';

        out << "info string capobs caplmr "
            << "late_simple " << cap_obs.cap_lmr_late_simple_total
            << " eligible " << cap_obs.cap_lmr_eligible
            << " considered " << cap_obs.cap_lmr_considered
            << " reduced " << cap_obs.cap_lmr_reduced
            << " (r1 " << cap_obs.cap_lmr_reduced_r1 << " r2 " << cap_obs.cap_lmr_reduced_r2 << ")"
            << " re_search " << cap_obs.cap_lmr_research
            << " (r1 " << cap_obs.cap_lmr_research_r1 << " r2 " << cap_obs.cap_lmr_research_r2 << ")\n";

        out << "info string capobs caplmr_by_see "
            << "considered bad " << cap_obs.cap_lmr_considered_see[0]
            << " eq " << cap_obs.cap_lmr_considered_see[1]
            << " good " << cap_obs.cap_lmr_considered_see[2]
            << " | reduced bad " << cap_obs.cap_lmr_reduced_see[0]
            << " eq " << cap_obs.cap_lmr_reduced_see[1]
            << " good " << cap_obs.cap_lmr_reduced_see[2]
            << " | re_search bad " << cap_obs.cap_lmr_research_see[0]
            << " eq " << cap_obs.cap_lmr_research_see[1]
            << " good " << cap_obs.cap_lmr_research_see[2] << '\n';

        const int sb_s_bad = sb_raw(DepthClass::Shallow, SeeClass::LossSmall);
        const int sb_s_eq = sb_raw(DepthClass::Shallow, SeeClass::Equal);
        const int sb_s_good_s = sb_raw(DepthClass::Shallow, SeeClass::GainSmall);
        const int sb_s_good_b = sb_raw(DepthClass::Shallow, SeeClass::GainBig);
        const int sb_ml_bad = sb_raw(DepthClass::MediumLow, SeeClass::LossSmall);
        const int sb_ml_eq = sb_raw(DepthClass::MediumLow, SeeClass::Equal);
        const int sb_ml_good_s = sb_raw(DepthClass::MediumLow, SeeClass::GainSmall);
        const int sb_ml_good_b = sb_raw(DepthClass::MediumLow, SeeClass::GainBig);
        const int sb_mh_bad = sb_raw(DepthClass::MediumHigh, SeeClass::LossSmall);
        const int sb_mh_eq = sb_raw(DepthClass::MediumHigh, SeeClass::Equal);
        const int sb_mh_good_s = sb_raw(DepthClass::MediumHigh, SeeClass::GainSmall);
        const int sb_mh_good_b = sb_raw(DepthClass::MediumHigh, SeeClass::GainBig);
        const int sb_d_bad = sb_raw(DepthClass::Deep, SeeClass::LossSmall);
        const int sb_d_eq = sb_raw(DepthClass::Deep, SeeClass::Equal);
        const int sb_d_good_s = sb_raw(DepthClass::Deep, SeeClass::GainSmall);
        const int sb_d_good_b = sb_raw(DepthClass::Deep, SeeClass::GainBig);

        out << "info string capobs see_bias_raw "
            << "shallow bad " << sb_s_bad << " eq " << sb_s_eq
            << " good_s " << sb_s_good_s << " good_b " << sb_s_good_b
            << " | med_low bad " << sb_ml_bad << " eq " << sb_ml_eq
            << " good_s " << sb_ml_good_s << " good_b " << sb_ml_good_b
            << " | med_high bad " << sb_mh_bad << " eq " << sb_mh_eq
            << " good_s " << sb_mh_good_s << " good_b " << sb_mh_good_b
            << " | deep bad " << sb_d_bad << " eq " << sb_d_eq
            << " good_s " << sb_d_good_s << " good_b " << sb_d_good_b << '\n';

        out << "info string capobs see_bias_term "
            << "shallow bad " << sb_term(sb_s_bad) << " eq " << sb_term(sb_s_eq)
            << " good_s " << sb_term(sb_s_good_s) << " good_b " << sb_term(sb_s_good_b)
            << " | med_low bad " << sb_term(sb_ml_bad) << " eq " << sb_term(sb_ml_eq)
            << " good_s " << sb_term(sb_ml_good_s) << " good_b " << sb_term(sb_ml_good_b)
            << " | med_high bad " << sb_term(sb_mh_bad) << " eq " << sb_term(sb_mh_eq)
            << " good_s " << sb_term(sb_mh_good_s) << " good_b " << sb_term(sb_mh_good_b)
            << " | deep bad " << sb_term(sb_d_bad) << " eq " << sb_term(sb_d_eq)
            << " good_s " << sb_term(sb_d_good_s) << " good_b " << sb_term(sb_d_good_b)
            << '\n';

        out << "info string capobs capture_hist_high "
            << "main " << cap_obs.main_high_hist_cutoff << '/' << cap_obs.main_high_hist_tried
            << " (" << ratio_percent(cap_obs.main_high_hist_cutoff, cap_obs.main_high_hist_tried) << "%) "
            << "q " << cap_obs.q_high_hist_cutoff << '/' << cap_obs.q_high_hist_tried
            << " (" << ratio_percent(cap_obs.q_high_hist_cutoff, cap_obs.q_high_hist_tried) << "%)";

        for (int i = 0; i < CAPTURE_TOPK; ++i) {
            const i64 sum = cap_obs.topk_rank_shift_sum[i];
            const u64 cnt = cap_obs.topk_rank_shift_count[i];
            const int avg_x100 = cnt == 0 ? 0 : static_cast<int>((sum * 100) / static_cast<i64>(cnt));
            out << " d" << (i + 1) << "_avg_rank_shift_x100 " << avg_x100;
        }
        out << '\n';
    }
#endif

#if VALERAIN_MOVEPICKER_OBS
    [[nodiscard]] static inline int mp_ratio_percent(u64 num, u64 den) noexcept {
        if (den == 0)
            return 0;
        return static_cast<int>((num * 100ULL) / den);
    }

    [[nodiscard]] static inline MoveStageBucket classify_stage_bucket(
        Move move,
        Move tt_move,
        Move killer1,
        Move killer2,
        bool capture_move,
        bool bad_capture
    ) noexcept {
        if (!move_is_none(tt_move) && move == tt_move)
            return MoveStageBucket::TT;
        if (capture_move)
            return bad_capture ? MoveStageBucket::BadCapture : MoveStageBucket::GoodCapture;
        if (move == killer1 || move == killer2)
            return MoveStageBucket::Killer;
        return MoveStageBucket::Quiet;
    }

    inline void emit_movepicker_observation(std::ostream& out) const {
        const u64 tt_probe_rate_num = mp_obs.nodes_with_tt_probe;
        const u64 tt_probe_rate_den = mp_obs.nodes;
        const u64 tt_first_rate_num = mp_obs.tt_first_try;
        const u64 tt_first_rate_den = mp_obs.nodes_with_tt_probe;
        const u64 tt_first_cut_num = mp_obs.tt_first_cutoff;
        const u64 tt_first_cut_den = mp_obs.tt_first_try;

        out << "info string mpobs nodes " << mp_obs.nodes
            << " tt_probe " << tt_probe_rate_num << '/' << tt_probe_rate_den
            << " (" << mp_ratio_percent(tt_probe_rate_num, tt_probe_rate_den) << "%)"
            << " tt_first_try " << tt_first_rate_num << '/' << tt_first_rate_den
            << " (" << mp_ratio_percent(tt_first_rate_num, tt_first_rate_den) << "%)"
            << " tt_first_cut " << tt_first_cut_num << '/' << tt_first_cut_den
            << " (" << mp_ratio_percent(tt_first_cut_num, tt_first_cut_den) << "%)\n";

        out << "info string mpobs cutoff_by_stage "
            << "total " << mp_obs.cutoffs_total
            << " tt " << mp_obs.cutoff_by_stage[static_cast<int>(MoveStageBucket::TT)]
            << " goodcap " << mp_obs.cutoff_by_stage[static_cast<int>(MoveStageBucket::GoodCapture)]
            << " killer " << mp_obs.cutoff_by_stage[static_cast<int>(MoveStageBucket::Killer)]
            << " quiet " << mp_obs.cutoff_by_stage[static_cast<int>(MoveStageBucket::Quiet)]
            << " badcap " << mp_obs.cutoff_by_stage[static_cast<int>(MoveStageBucket::BadCapture)]
            << '\n';

        out << "info string mpobs first_stage_cutoff_rate "
            << "goodcap " << mp_obs.first_good_capture_cutoff << '/' << mp_obs.first_good_capture_try
            << " (" << mp_ratio_percent(mp_obs.first_good_capture_cutoff, mp_obs.first_good_capture_try) << "%) "
            << "killer " << mp_obs.first_killer_cutoff << '/' << mp_obs.first_killer_try
            << " (" << mp_ratio_percent(mp_obs.first_killer_cutoff, mp_obs.first_killer_try) << "%) "
            << "quiet " << mp_obs.first_quiet_cutoff << '/' << mp_obs.first_quiet_try
            << " (" << mp_ratio_percent(mp_obs.first_quiet_cutoff, mp_obs.first_quiet_try) << "%)\n";

        out << "info string mpobs quiet_work "
            << "generated " << mp_obs.quiet_generated
            << " scored " << mp_obs.quiet_scored
            << " quiet_skipped_by_mp " << mp_obs.quiet_skipped_by_mp
            << " searched " << mp_obs.quiet_searched << '\n';

        out << "info string mpobs quiet_failhigh "
            << "late_quiet_fail_high " << mp_obs.late_quiet_fail_high
            << " quiet_fail_high_after_skip_band " << mp_obs.quiet_fail_high_after_skip_band
            << '\n';
    }
#endif

#if VALERAIN_SEARCH_OBS
    [[nodiscard]] static inline int search_ratio_percent(u64 num, u64 den) noexcept {
        if (den == 0)
            return 0;
        return static_cast<int>((num * 100ULL) / den);
    }

    inline void emit_search_observation(std::ostream& out) const {
        out << "info string searchobs nmp candidates "
            << search_obs.nmp_candidates
            << " tried " << search_obs.nmp_tried
            << " failhigh " << search_obs.nmp_fail_high
            << " verify " << search_obs.nmp_verification_tried
            << " verified " << search_obs.nmp_verified_cutoffs
            << " failed " << search_obs.nmp_verification_failed
            << '\n';
        out << "info string searchobs probcut_nodes "
            << search_obs.probcut_nodes
            << " probcut_moves " << search_obs.probcut_moves
            << " probcut_cutoffs " << search_obs.probcut_cutoffs
            << " (" << search_ratio_percent(search_obs.probcut_cutoffs, search_obs.probcut_moves) << "%)\n";
        out << "info string searchobs singular "
            << search_obs.singular_extend1 << '/' << search_obs.singular_candidates
            << " (" << search_ratio_percent(search_obs.singular_extend1, search_obs.singular_candidates) << "%)"
            << " double " << search_obs.singular_extend2 << '\n';
    }
#endif

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

    [[nodiscard]] static inline memory::Bound tt_bound_from_probe(
        const memory::TTProbe& probe
    ) noexcept {
        return probe.hit
            ? static_cast<memory::Bound>(probe.data.flags & 0x3U)
            : memory::BOUND_NONE;
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

    [[nodiscard]] static inline int reverse_futility_margin(
        int depth,
        bool improving,
        int correction,
        Move tt_move,
        memory::Bound tt_bound,
        int tt_score,
        int beta
    ) noexcept {
        int margin = RFP_BASE_MARGIN
            + depth * RFP_DEPTH_MARGIN
            - (improving ? RFP_IMPROVING_MARGIN : 0);

        if (correction > RFP_CORRECTION_THRESHOLD)
            margin += RFP_CORRECTION_MARGIN_BONUS;

        if (!move_is_none(tt_move) && move_is_capture(tt_move)) {
            margin = std::max(0, margin - RFP_TT_CAPTURE_MARGIN_REDUCTION);
        } else if (!move_is_none(tt_move) &&
                   (tt_bound == memory::BOUND_LOWER || tt_bound == memory::BOUND_EXACT) &&
                   tt_score >= beta) {
            margin += RFP_TT_QUIET_FAIL_HIGH_BONUS;
        }

        return margin;
    }

    [[nodiscard]] static inline int futility_margin(
        int depth,
        bool improving,
        int history_score
    ) noexcept {
        return FUTILITY_BASE_MARGIN
            + depth * FUTILITY_DEPTH_MARGIN
            - (improving ? FUTILITY_IMPROVING_MARGIN : 0)
            + std::clamp(
                history_score / FUTILITY_HISTORY_DIVISOR,
                -64,
                64
            );
    }

    [[nodiscard]] static inline int lmp_limit(int depth, bool improving) noexcept {
        const int d = std::clamp(depth, 1, 11);
        if (improving)
            return 4 + (4 * d * d) / 5;
        return 2 + (2 * d * d) / 5;
    }

    [[nodiscard]] static inline int history_prune_threshold(
        int depth,
        bool improving
    ) noexcept {
        const int coeff = improving ? 5 : 3;
        return -depth * depth * coeff;
    }

    [[nodiscard]] static inline QuietControl quiet_control_for_node(
        int depth,
        bool improving,
        int static_eval,
        int alpha,
        Move tt_move,
        int node_history_signal
    ) noexcept {
        QuietControl control{};
        if (depth < 5)
            return control;

        control.skip_quiets = true;
        control.keep_top_history = 4;
        control.quiet_limit = std::max(4, lmp_limit(depth, improving) / 2);

        if (!improving)
            control.quiet_limit = std::max(3, control.quiet_limit - 1);
        if (static_eval <= alpha)
            control.quiet_limit = std::max(3, control.quiet_limit - 1);
        if (node_history_signal < 0)
            control.quiet_limit = std::max(3, control.quiet_limit - 1);

        if (!move_is_none(tt_move) && !move_is_capture(tt_move)) {
            ++control.quiet_limit;
        } else if (!move_is_none(tt_move)) {
            control.quiet_limit = std::max(3, control.quiet_limit - 1);
        }

        control.history_floor = history_prune_threshold(depth, improving) - 64;
        if (static_eval <= alpha)
            control.history_floor += 32;
        if (!improving)
            control.history_floor += 16;
        if (node_history_signal < 0)
            control.history_floor += std::min(32, -node_history_signal);
        if (!move_is_none(tt_move) && !move_is_capture(tt_move))
            control.history_floor -= 32;

        control.history_floor = std::clamp(control.history_floor, -512, -32);
        return control;
    }

    [[nodiscard]] inline u64 global_nodes() const noexcept {
        return base_nodes + nodes;
    }

    inline void update_seldepth(int ply) noexcept {
        if (ply > seldepth)
            seldepth = ply;
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

    [[nodiscard]] inline bool is_recapture_move(Move move, int ply) const noexcept {
        if (ply <= 0)
            return false;
        const Move prev = move_stack[ply - 1];
        if (move_is_none(prev))
            return false;
        return to_sq(move) == to_sq(prev);
    }

    [[nodiscard]] inline bool gives_check_after_move(
        const Position& pos,
        Move move
    ) const noexcept {
        return move_gives_check(pos, mem, move);
    }

    [[nodiscard]] inline bool use_nnue() const noexcept {
        return limits.use_nnue && nnue::loaded();
    }

    [[nodiscard]] inline int evaluate_position(const Position& pos) const noexcept {
        if (use_nnue())
            return nnue::search_score(nnue::eval(pos), pos);
        return eval::evaluate(pos);
    }

    [[nodiscard]] static inline bool tt_eval_available(
        const memory::TTProbe& probe
    ) noexcept {
        return probe.hit && probe.data.eval != VALUE_NONE;
    }

    [[nodiscard]] static inline std::size_t correction_index(Key key) noexcept {
        return static_cast<std::size_t>(mix64(key)) & (CORRECTION_HISTORY_SIZE - 1);
    }

    [[nodiscard]] static inline int packed_piece_count(
        const Position& pos,
        Color color,
        PieceType piece_type
    ) noexcept {
        return std::popcount(static_cast<u64>(pieces(pos, color, piece_type)));
    }

    [[nodiscard]] CorrectionKeys correction_keys(const Position& pos) const noexcept {
        CorrectionKeys keys{};
        keys.position = pos.key;
        keys.pawn_king =
            pieces(pos, WHITE, PAWN)
            ^ std::rotl(pieces(pos, BLACK, PAWN), 7)
            ^ std::rotl(pieces(pos, WHITE, KING), 19)
            ^ std::rotl(pieces(pos, BLACK, KING), 29);

        u64 material = 0;
        int shift = 0;
        constexpr PieceType material_piece_types[] = {
            PAWN, KNIGHT, BISHOP, ROOK, QUEEN
        };
        for (int color = WHITE; color <= BLACK; ++color) {
            const Color piece_color = static_cast<Color>(color);
            for (PieceType piece_type : material_piece_types) {
                material |= static_cast<u64>(packed_piece_count(pos, piece_color, piece_type)) << shift;
                shift += 4;
            }
        }
        keys.material = material;
        return keys;
    }

    [[nodiscard]] static inline int correction_slot_value(
        const i16 table[CORRECTION_HISTORY_SIZE],
        Key key
    ) noexcept {
        return static_cast<int>(table[correction_index(key)]);
    }

    inline void update_correction_slot(
        i16& slot,
        int target,
        int weight
    ) noexcept {
        const int next =
            static_cast<int>(slot)
            + ((target - static_cast<int>(slot)) * weight) / 128;
        slot = static_cast<i16>(std::clamp(
            next,
            -CORRECTION_HISTORY_CLAMP * CORRECTION_HISTORY_GRAIN,
            CORRECTION_HISTORY_CLAMP * CORRECTION_HISTORY_GRAIN
        ));
    }

    [[nodiscard]] inline int correction_value(
        Color side,
        const CorrectionKeys& keys
    ) const noexcept {
        const int stored =
            CORRECTION_POSITION_WEIGHT
                * correction_slot_value(position_correction_history[side], keys.position)
            + CORRECTION_PAWN_WEIGHT
                * correction_slot_value(pawn_correction_history[side], keys.pawn_king)
            + CORRECTION_MATERIAL_WEIGHT
                * correction_slot_value(material_correction_history[side], keys.material);
        return std::clamp(
            stored / (CORRECTION_HISTORY_GRAIN * CORRECTION_WEIGHT_SUM),
            -CORRECTION_HISTORY_CLAMP,
            CORRECTION_HISTORY_CLAMP
        );
    }

    inline void update_correction_history(
        Color side,
        const CorrectionKeys& keys,
        int raw_eval,
        int score,
        int depth
    ) noexcept {
        if (raw_eval == VALUE_NONE || is_mate_window(score))
            return;

        const int delta = std::clamp(
            score - raw_eval,
            -CORRECTION_HISTORY_CLAMP,
            CORRECTION_HISTORY_CLAMP
        );
        const int target = delta * CORRECTION_HISTORY_GRAIN;
        const int weight = std::min(
            CORRECTION_HISTORY_WEIGHT_MAX,
            16 + std::max(1, depth) * 8
        );
        update_correction_slot(
            position_correction_history[side][correction_index(keys.position)],
            target,
            weight
        );
        update_correction_slot(
            pawn_correction_history[side][correction_index(keys.pawn_king)],
            target,
            weight
        );
        update_correction_slot(
            material_correction_history[side][correction_index(keys.material)],
            target,
            weight
        );
    }

    inline void store_static_eval(int ply, int static_eval) noexcept {
        static_eval_stack[ply] = static_eval;
        static_eval_valid[ply] = true;
    }

    [[nodiscard]] inline bool improving_position(
        int ply,
        int static_eval
    ) const noexcept {
        if (ply < 2 || !static_eval_valid[ply - 2])
            return false;
        return static_eval >= static_eval_stack[ply - 2] + IMPROVING_MARGIN;
    }

    [[nodiscard]] bool is_repetition_draw(
        const Position& pos,
        int ply
    ) const noexcept {
        int matches = 1;
        const int history_count = std::min(limits.game_history_count, pos.halfmove_clock);
        const int history_start = limits.game_history_count - history_count;
        for (int i = history_start; i < limits.game_history_count; ++i) {
            if (limits.game_history_keys[i] != pos.key)
                continue;
            if (++matches >= 3)
                return true;
        }

        const int back = std::min(ply, pos.halfmove_clock);
        const int min_ply = ply - back;

        for (int p = ply - 2; p >= min_ply; p -= 2) {
            if (rep_keys[p] != pos.key)
                continue;
            if (++matches >= 3)
                return true;
        }

        return false;
    }

    [[nodiscard]] inline int repetition_score(int eval_hint) const noexcept {
        if (eval_hint == VALUE_NONE)
            return 0;
        return eval_hint > 0 ? REPETITION_AVOID_SCORE : 0;
    }

    [[nodiscard]] bool has_null_move_pruning_material(
        const Position& pos,
        Color side
    ) const noexcept {
        if (pieces(pos, side, QUEEN) != 0ULL || pieces(pos, side, ROOK) != 0ULL)
            return true;

        const Bitboard minors =
            pieces(pos, side, KNIGHT) |
            pieces(pos, side, BISHOP);
        if (minors == 0ULL)
            return false;

        return (minors & (minors - 1)) != 0ULL || pieces(pos, side, PAWN) != 0ULL;
    }

    struct NullMoveState {
        int side_to_move;
        Square ep_sq;
        int halfmove_clock;
        int fullmove_number;
        Key key;
    };

    inline void do_null_move(Position& pos, NullMoveState& st) const noexcept {
        // A null move simply passes the turn, clears ep state, and updates the
        // key. It is used for null-move pruning only; no board pieces move.
        st.side_to_move = pos.side_to_move;
        st.ep_sq = pos.ep_sq;
        st.halfmove_clock = pos.halfmove_clock;
        st.fullmove_number = pos.fullmove_number;
        st.key = pos.key;

        if (has_ep(pos))
            pos.key ^= mem.tables.zobrist.ep_file[file_of(pos.ep_sq)];

        if (pos.side_to_move == BLACK)
            ++pos.fullmove_number;

        ++pos.halfmove_clock;
        pos.ep_sq = NO_SQ;
        pos.side_to_move ^= 1;
        pos.key ^= mem.tables.zobrist.side;
    }

    inline void undo_null_move(Position& pos, const NullMoveState& st) const noexcept {
        pos.side_to_move = st.side_to_move;
        pos.ep_sq = st.ep_sq;
        pos.halfmove_clock = st.halfmove_clock;
        pos.fullmove_number = st.fullmove_number;
        pos.key = st.key;
    }

    [[nodiscard]] inline Move tt_move_from_probe(
        const memory::TTProbe& probe
    ) const noexcept {
        return probe.hit ? static_cast<Move>(probe.data.move) : Move(0);
    }

    [[nodiscard]] inline int tt_eval_from_probe(
        const memory::TTProbe& probe
    ) const noexcept {
        return tt_eval_available(probe) ? probe.data.eval : VALUE_NONE;
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
        int tt_eval,
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
            static_cast<i16>(tt_eval),
            static_cast<i16>(depth),
            bound_from_score(score, alpha, beta),
            pv_node
        );
    }

    [[nodiscard]] StaticEvalInfo resolve_static_eval(
        const Position& pos,
        const memory::TTProbe& probe,
        int ply,
        bool checked,
        bool qsearch_node
    ) const noexcept {
        StaticEvalInfo info{};
        info.keys = correction_keys(pos);
        const Color side = static_cast<Color>(pos.side_to_move);
        info.raw = tt_eval_available(probe) ? probe.data.eval : evaluate_position(pos);
        info.corrected = std::clamp(
            info.raw + correction_value(side, info.keys),
            -VALUE_INF,
            VALUE_INF
        );
        info.stand_pat = info.corrected;

        if (!qsearch_node || checked || !probe.hit)
            return info;

        const int tt_score = score_from_tt(probe.data.score, ply);
        if (is_mate_window(tt_score))
            return info;

        switch (tt_bound_from_probe(probe)) {
            case memory::BOUND_LOWER: {
                const int gap = tt_score - info.stand_pat;
                if (gap > 0)
                    info.stand_pat += std::min(QS_ADJ_SHUFFLE_CAP, std::max(1, gap / 2));
                break;
            }
            case memory::BOUND_UPPER: {
                const int gap = info.stand_pat - tt_score;
                if (gap > 0)
                    info.stand_pat -= std::min(QS_ADJ_SHUFFLE_CAP, std::max(1, gap / 2));
                break;
            }
            default:
                break;
        }

        return info;
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

    [[nodiscard]] inline i32 score_move(
        const Position& pos,
        Move move,
        Move tt_move,
        int ply,
        int depth
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
            const int mvv_lva_term = mvv_lva_capture_term(pos, move);
            const int see_value = search::see_value(pos, mem, move);
            const int immediate_see_term = see_immediate_term(see_value, SEE_TERM_PRESET);
            const int see_bias_term = history_tables.see_bias_value_fast(depth, see_value);
            return 20'000'000 + mvv_lva_term
                + history_tables.capture_value_fast(pos, move)
                + immediate_see_term
                + see_bias_term;
        }

        if (move_is_promotion(move))
            return 19'000'000 + piece_order_value[promo_piece(move)];

        if (move == history_tables.killer_fast(ply, 0))
            return 18'000'000;
        if (move == history_tables.killer_fast(ply, 1))
            return 17'999'000;

        return history_tables.quiet_value_fast(pos, move);
    }

    inline void score_moves(
        const Position& pos,
        const MoveList& moves,
        ScoredMoveList& scored,
        Move tt_move,
        int ply,
        int depth
    ) const noexcept {
        scored.size = moves.size;
        for (int i = 0; i < moves.size; ++i) {
            scored.moves[i].move = moves.moves[i];
            scored.moves[i].score = score_move(pos, moves.moves[i], tt_move, ply, depth);
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
        update_seldepth(ply);
        ++nodes;
        poll_limits();
        if (stopped)
            return alpha;

        if (ply >= MAX_PLY - 1)
            return evaluate_position(pos);

        if (pos.halfmove_clock >= 100)
            return 0;

        alpha = std::max(alpha, -VALUE_MATE + ply);
        beta = std::min(beta, VALUE_MATE - ply - 1);
        if (alpha >= beta)
            return alpha;

        const int alpha0 = alpha;
        const bool pv_node = (beta - alpha) > 1;
        const memory::TTProbe probe = memory::tt_probe(mem.tt, pos.key);
        if (is_repetition_draw(pos, ply))
            return repetition_score(tt_eval_from_probe(probe));

        int tt_score = 0;
        if (tt_cutoff(probe, 0, alpha, beta, ply, tt_score))
            return tt_score;

        const bool checked = in_check(pos);
        const Move tt_move = tt_move_from_probe(probe);
        const StaticEvalInfo eval_info = resolve_static_eval(pos, probe, ply, checked, true);
        const int raw_eval = eval_info.raw;
        const int static_eval = eval_info.corrected;
        const int stand_pat_eval = eval_info.stand_pat;
        store_static_eval(ply, static_eval);

        if (!checked) {
            // Stand-pat: if the static position already fails high, no capture
            // search can make it worse for the side to move.
            if (stand_pat_eval >= beta) {
                save_tt(pos, 0, ply, stand_pat_eval, raw_eval, tt_move, alpha0, beta, pv_node);
                return stand_pat_eval;
            }
            if (stand_pat_eval > alpha)
                alpha = stand_pat_eval;
        }

        MoveList list;
        GenInfo info;
        init_gen_info(info, pos, mem);
        Move* qend = generate_pseudo_captures(pos, mem, info, list.moves);
        list.size = static_cast<int>(qend - list.moves);

        if (list.size == 0) {
            const int score = checked ? (-VALUE_MATE + ply) : alpha;
            save_tt(pos, 0, ply, score, raw_eval, 0, alpha0, beta, pv_node);
            return score;
        }

        ScoredMoveList scored;
        score_moves(pos, list, scored, tt_move, ply, 0);

        Move best_move = 0;
        int legal_count = 0;
        for (int i = 0; i < scored.size; ++i) {
            const Move move = pick_next(scored, i);
            if (!legal_fast(pos, mem, info, move))
                continue;

            ++legal_count;

            if (!checked &&
                !move_is_promotion(move) &&
                !search::see_ge(pos, mem, move, -DELTA_MARGIN)) {
                continue;
            }

            if (!checked && !move_is_promotion(move)) {
                // Delta pruning skips captures that cannot reasonably raise alpha.
                const int max_gain = capture_gain_estimate(pos, move);
                if (static_eval + max_gain + DELTA_MARGIN <= alpha)
                    continue;
            }

#if VALERAIN_CAPTURE_OBS
            const bool capture_move = move_is_capture(move);
            int obs_see_value = 0;
            int obs_capture_hist = 0;
            if (capture_move) {
                obs_see_value = search::see_value_fast(pos, mem, move);
                obs_capture_hist = history_tables.capture_value_fast(pos, move);
                record_q_capture_try(obs_see_value, obs_capture_hist);
            }
#endif

            StateInfo st;
            make_move(pos, move, mem.tables, st);
            memory::tt_prefetch(mem.tt, pos.key);

            const int score = -qsearch(pos, -beta, -alpha, ply + 1);
            unmake_move(pos, move, mem.tables, st);
            if (score > alpha) {
                alpha = score;
                best_move = move;
                update_pv(ply, move);
                if (alpha >= beta) {
#if VALERAIN_CAPTURE_OBS
                    if (move_is_capture(move))
                        record_q_capture_cutoff(obs_see_value, obs_capture_hist);
#endif
                    break;
                }
            }
        }

        if (legal_count == 0) {
            const int score = checked ? (-VALUE_MATE + ply) : alpha;
            save_tt(pos, 0, ply, score, raw_eval, 0, alpha0, beta, pv_node);
            return score;
        }

        save_tt(pos, 0, ply, alpha, raw_eval, best_move, alpha0, beta, pv_node);
        return alpha;
    }

    [[nodiscard]] int pvs(
        Position& pos,
        int depth,
        int alpha,
        int beta,
        int ply,
        bool allow_null,
        Move excluded_move = Move(0)
    ) noexcept {
        // Principal Variation Search:
        // - first move gets a full window
        // - later moves get a null window first
        // - promising moves are re-searched on a wider window
        pv_length[ply] = 0;
        rep_keys[ply] = pos.key;
        update_seldepth(ply);

        if (stopped)
            return alpha;

        if (ply >= MAX_PLY - 1)
            return evaluate_position(pos);

        if (pos.halfmove_clock >= 100)
            return 0;

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
        const bool exclusion_search = !move_is_none(excluded_move);
        const memory::TTProbe probe = memory::tt_probe(mem.tt, pos.key);
        if (is_repetition_draw(pos, ply))
            return repetition_score(tt_eval_from_probe(probe));
        const Move probed_tt_move = tt_move_from_probe(probe);
        const Move tt_move = exclusion_search ? Move(0) : probed_tt_move;
        const memory::Bound tt_bound = tt_bound_from_probe(probe);
        const int probed_tt_score = probe.hit ? score_from_tt(probe.data.score, ply) : 0;

        int search_depth = depth;
        if (!pv_node &&
            search_depth >= IIR_MIN_DEPTH &&
            move_is_none(tt_move) &&
            !exclusion_search) {
            --search_depth;
        }

        int tt_score = 0;
        if (!exclusion_search &&
            tt_cutoff(probe, search_depth, alpha, beta, ply, tt_score))
            return tt_score;

        const bool checked = in_check(pos);
        const StaticEvalInfo eval_info = resolve_static_eval(pos, probe, ply, checked, false);
        const int raw_eval = eval_info.raw;
        const int static_eval = eval_info.corrected;
        const int correction = static_eval - raw_eval;
        store_static_eval(ply, static_eval);
        const bool improving = !checked && improving_position(ply, static_eval);
        const Color side = static_cast<Color>(pos.side_to_move);
        SearchStackEntry& ss = search_stack[ply];
        ss.current_move = Move(0);
        ss.static_eval = static_eval;
        ss.stat_score = 0;
        ss.reduction_fp = 0;
        ss.move_count = 0;
        ss.cutoff_count = 0;
        ss.in_check = checked;
        ss.tt_hit = probe.hit;
        ss.tt_pv = pv_node;

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
            search_depth <= 6 &&
            !is_mate_window(beta) &&
            static_eval - reverse_futility_margin(
                search_depth,
                improving,
                correction,
                tt_move,
                tt_bound,
                probed_tt_score,
                beta
            ) >= beta) {
            // Reverse futility: when static eval is already safely above beta,
            // a shallow node often does not need a full search.
            return beta + (static_eval - beta) / 3;
        }

        const bool nmp_disabled_here = nmp_disabled_for_ply(ply, nmp_min_ply);
#if VALERAIN_SEARCH_OBS
        if (allow_null &&
            !pv_node &&
            !checked &&
            search_depth >= 3 &&
            !exclusion_search &&
            !is_mate_window(beta)) {
            ++search_obs.nmp_candidates;
        }
#endif

        NmpNodeContext nmp_node{};
        nmp_node.depth = search_depth;
        nmp_node.ply = ply;
        nmp_node.alpha = alpha;
        nmp_node.beta = beta;
        nmp_node.static_eval = static_eval;
        nmp_node.tt_score = probed_tt_score;
        nmp_node.nmp_min_ply = nmp_min_ply;
        nmp_node.allow_null = allow_null;
        nmp_node.pv_node = pv_node;
        nmp_node.cut_node =
            !pv_node &&
            (tt_bound == memory::BOUND_LOWER || !move_is_none(tt_move));
        nmp_node.checked = checked;
        nmp_node.improving = improving;
        nmp_node.exclusion_search = exclusion_search;
        nmp_node.tt_hit = probe.hit;
        nmp_node.tt_move_present = !move_is_none(tt_move);
        nmp_node.material_ok = has_null_move_pruning_material(pos, side);
        nmp_node.tt_bound = tt_bound;

        const NmpDecision nmp = decide_null_move(nmp_node);
        if (nmp.eligible && !is_mate_window(beta) && !nmp_disabled_here) {
            // Null-move pruning tests whether simply passing still keeps the
            // position above beta. If so, the real position is likely also a cutoff.
#if VALERAIN_SEARCH_OBS
            ++search_obs.nmp_tried;
#endif
            NullMoveState null_state;
            do_null_move(pos, null_state);
            move_stack[ply] = Move(0);

            const int score = -pvs(
                pos,
                nmp.null_depth,
                -beta,
                -beta + 1,
                ply + 1,
                false
            );
            undo_null_move(pos, null_state);

            if (score >= beta && !is_mate_window(score)) {
#if VALERAIN_SEARCH_OBS
                ++search_obs.nmp_fail_high;
#endif
                if (!nmp.requires_verification) {
                    save_tt(pos, search_depth, ply, score, raw_eval, 0, alpha0, beta, pv_node);
                    return score;
                }

#if VALERAIN_SEARCH_OBS
                ++search_obs.nmp_verification_tried;
#endif
                const int old_nmp_min_ply = nmp_min_ply;
                nmp_min_ply = nmp.verify_min_ply;
                const int verify_score = pvs(
                    pos,
                    nmp.verify_depth,
                    beta - 1,
                    beta,
                    ply,
                    true
                );
                nmp_min_ply = old_nmp_min_ply;

                if (verify_score >= beta) {
#if VALERAIN_SEARCH_OBS
                    ++search_obs.nmp_verified_cutoffs;
#endif
                    save_tt(pos, search_depth, ply, score, raw_eval, 0, alpha0, beta, pv_node);
                    return score;
                }

#if VALERAIN_SEARCH_OBS
                ++search_obs.nmp_verification_failed;
#endif
            }
        }

        GenInfo info;
        init_gen_info(info, pos, mem);

#if VALERAIN_ENABLE_PROBCUT
        if (!pv_node &&
            !checked &&
            !exclusion_search &&
            search_depth >= PROBCUT_MIN_DEPTH &&
            !is_mate_window(beta)) {
#if VALERAIN_SEARCH_OBS
            ++search_obs.probcut_nodes;
#endif
            const int probcut_beta = beta + PROBCUT_MARGIN;

            if (probe.hit &&
                tt_bound == memory::BOUND_LOWER &&
                probe.data.depth >= search_depth - PROBCUT_TT_DEPTH_MARGIN &&
                probed_tt_score >= probcut_beta &&
                !is_mate_window(probed_tt_score)) {
                return probed_tt_score;
            }

            MoveList probcut_moves{};
            Move* probcut_end = generate_pseudo_captures_only(pos, mem, info, probcut_moves.moves);
            probcut_moves.size = static_cast<int>(probcut_end - probcut_moves.moves);

            if (probcut_moves.size > 0) {
                ScoredMoveList scored_probcut{};
                score_moves(pos, probcut_moves, scored_probcut, tt_move, ply, search_depth);

                for (int i = 0; i < scored_probcut.size; ++i) {
                    const Move move = pick_next(scored_probcut, i);
                    if (!move_is_capture(move))
                        continue;
                    if (!legal_fast(pos, mem, info, move))
                        continue;
                    if (!search::see_ge_fast(pos, mem, move, 0))
                        continue;
#if VALERAIN_SEARCH_OBS
                    ++search_obs.probcut_moves;
#endif

                    StateInfo st;
                    make_move(pos, move, mem.tables, st);
                    memory::tt_prefetch(mem.tt, pos.key);

                    int score = -qsearch(pos, -probcut_beta, -probcut_beta + 1, ply + 1);
                    if (score >= probcut_beta) {
                        const int probcut_depth = std::max(0, search_depth - PROBCUT_REDUCTION);
                        if (probcut_depth > 0) {
                            score = -pvs(
                                pos,
                                probcut_depth,
                                -probcut_beta,
                                -probcut_beta + 1,
                                ply + 1,
                                false
                            );
                        }
                    }

                    unmake_move(pos, move, mem.tables, st);

                    if (score >= probcut_beta) {
#if VALERAIN_SEARCH_OBS
                        ++search_obs.probcut_cutoffs;
#endif
                        save_tt(pos, search_depth - 3, ply, score, raw_eval, move, alpha0, beta, pv_node);
                        return score;
                    }
                }
            }
        }
#endif

        const Move prev_move = (ply > 0) ? move_stack[ply - 1] : Move(0);
        const Move prev2_move = (ply > 1) ? move_stack[ply - 2] : Move(0);
        QuietControl quiet_control{};
        if (!pv_node && !checked) {
            int node_history_signal = 0;
            if (ply > 0)
                node_history_signal += search_stack[ply - 1].stat_score / 256;
            if (ply > 1)
                node_history_signal += search_stack[ply - 2].stat_score / 512;

            quiet_control = quiet_control_for_node(
                search_depth,
                improving,
                static_eval,
                alpha,
                tt_move,
                node_history_signal
            );
        }
        MovePicker picker(
            pos,
            mem,
            info,
            history_tables,
            tt_move,
            ply,
            prev_move,
            prev2_move,
            search_depth,
            quiet_control
        );
#if VALERAIN_MOVEPICKER_OBS
        ++mp_obs.nodes;
        if (!move_is_none(tt_move))
            ++mp_obs.nodes_with_tt_probe;
        const Move killer1 = history_tables.killer_fast(ply, 0);
        const Move killer2 = history_tables.killer_fast(ply, 1);
        bool seen_first_good_capture = false;
        bool seen_first_killer = false;
        bool seen_first_quiet = false;
#endif

#if VALERAIN_CAPTURE_OBS
        Move capture_moves[MAX_MOVES];
        int capture_base_scores[MAX_MOVES];
        int capture_base_ranks[MAX_MOVES];
        int capture_count = 0;
        MoveList capture_list;
        Move* cend = generate_pseudo_captures(pos, mem, info, capture_list.moves);
        capture_list.size = static_cast<int>(cend - capture_list.moves);
        for (int i = 0; i < capture_list.size; ++i) {
            const Move move = capture_list.moves[i];
            if (!move_is_capture(move))
                continue;
            if (!legal_fast(pos, mem, info, move))
                continue;
            capture_moves[capture_count] = move;
            capture_base_scores[capture_count] = mvv_lva_capture_term(pos, move);
            ++capture_count;
        }
        for (int i = 0; i < capture_count; ++i) {
            int rank = 1;
            for (int j = 0; j < capture_count; ++j)
                if (capture_base_scores[j] > capture_base_scores[i])
                    ++rank;
            capture_base_ranks[i] = rank;
        }
        int capture_search_order = 0;
#endif

        bool searched_first = false;
        Move best_move = 0;
        int legal_count = 0;
        int quiet_count = 0;
        Move searched_quiets[MAX_MOVES];
        int searched_quiet_count = 0;
        Move searched_captures[MAX_MOVES];
        int searched_capture_see[MAX_MOVES];
        int searched_capture_count = 0;
        int simple_capture_count = 0;
        int moves_tried = 0;
        bool cutoff = false;

        for (;;) {
            if (stopped)
                break;

            const Move move = picker.next();
            if (move_is_none(move))
                break;
            if (move == excluded_move)
                continue;

            ++moves_tried;
            const int move_index = moves_tried - 1;
            ++legal_count;
            const bool capture_move = move_is_capture(move);
            [[maybe_unused]] const bool bad_capture = picker.last_was_bad_capture();
            const bool quiet_move =
                !capture_move &&
                !move_is_promotion(move) &&
                !move_is_castle(move);
            const bool simple_capture =
                capture_move &&
                !move_is_promotion(move);
            int move_see_value = 0;
            if (capture_move)
                move_see_value = picker.last_see_value();
            const int history_score = quiet_move
                ? history_tables.quiet_value_fast(pos, move)
                : 0;
            const int quiet_ordering_score = quiet_move
                ? picker.last_score()
                : 0;
            const int capture_history_score = simple_capture
                ? history_tables.capture_value_fast(pos, move)
                : 0;
            const bool lmr_quiet_candidate =
                quiet_move &&
                !pv_node &&
                !checked &&
                search_depth >= 3 &&
                move_index >= 3;
            const bool lmr_capture_candidate =
                simple_capture &&
                !pv_node &&
                !checked &&
                search_depth >= 4 &&
                (simple_capture_count - 1) >= 2;
            bool gives_check = false;
            bool gives_check_known = false;
            const auto ensure_gives_check = [&]() noexcept {
                if (!gives_check_known) {
                    gives_check = gives_check_after_move(pos, move);
                    gives_check_known = true;
                }
                return gives_check;
            };
#if VALERAIN_MOVEPICKER_OBS
            const bool first_move_this = (moves_tried == 1);
            const MoveStageBucket stage_bucket = classify_stage_bucket(
                move, tt_move, killer1, killer2, capture_move, bad_capture
            );
            const bool first_good_capture_this =
                stage_bucket == MoveStageBucket::GoodCapture && !seen_first_good_capture;
            const bool first_killer_this =
                stage_bucket == MoveStageBucket::Killer && !seen_first_killer;
            const bool first_quiet_this =
                stage_bucket == MoveStageBucket::Quiet && !seen_first_quiet;
            if (first_move_this && stage_bucket == MoveStageBucket::TT)
                ++mp_obs.tt_first_try;
            if (first_good_capture_this) {
                seen_first_good_capture = true;
                ++mp_obs.first_good_capture_try;
            }
            if (first_killer_this) {
                seen_first_killer = true;
                ++mp_obs.first_killer_try;
            }
            if (first_quiet_this) {
                seen_first_quiet = true;
                ++mp_obs.first_quiet_try;
            }
#endif

            if (quiet_move)
                ++quiet_count;
            if (simple_capture)
                ++simple_capture_count;

#if VALERAIN_SEE_LATE_BAD_CAPTURE_GATE
            if (!pv_node &&
                !checked &&
                simple_capture &&
                search_depth >= SEE_LATE_BAD_CAPTURE_GATE_MIN_DEPTH &&
                search_depth <= SEE_LATE_BAD_CAPTURE_GATE_MAX_DEPTH &&
                simple_capture_count >= SEE_LATE_BAD_CAPTURE_GATE_MIN_CAPTURE_INDEX) {
                const bool bad_see = move_see_value < SEE_LATE_BAD_CAPTURE_GATE_THRESHOLD;
                [[maybe_unused]] bool pruned = false;
                bool exempt_check = false;
                bool exempt_recapture = false;

                if (bad_see) {
                    exempt_recapture = is_recapture_move(move, ply);
                    if (!exempt_recapture)
                        exempt_check = ensure_gives_check();

                    if (!exempt_recapture && !exempt_check) {
                        pruned = true;
#if VALERAIN_CAPTURE_OBS
                        record_gate_check(bad_see, pruned, exempt_check, exempt_recapture);
#endif
                        continue;
                    }
                }
#if VALERAIN_CAPTURE_OBS
                record_gate_check(bad_see, pruned, exempt_check, exempt_recapture);
#endif
            }
#endif

            if (!pv_node &&
                !checked &&
                search_depth <= SEE_PRUNE_DEPTH_LIMIT &&
                simple_capture &&
                move_index > 1 &&
                !search::see_ge(pos, mem, move, -60 * search_depth)) {
                // Bad capture pruning: skip late captures that fail a SEE threshold.
                continue;
            }

            if (!pv_node &&
                !checked &&
                search_depth <= 4 &&
                quiet_move &&
                !ensure_gives_check() &&
                static_eval + futility_margin(search_depth, improving, history_score) <= alpha) {
                // Shallow futility pruning skips quiet moves that cannot raise alpha.
                continue;
            }

            if (!pv_node &&
                !checked &&
                search_depth <= 7 &&
                quiet_move &&
                !ensure_gives_check() &&
                quiet_count > lmp_limit(search_depth, improving) &&
                static_eval <= alpha) {
                // Late move pruning drops very late quiets once enough earlier
                // quiets have already been searched with no improvement.
                continue;
            }

            if (!pv_node &&
                !checked &&
                search_depth <= 6 &&
                quiet_move &&
                !ensure_gives_check() &&
                quiet_count > std::max(2, lmp_limit(search_depth, improving) / 2) &&
                history_score <= history_prune_threshold(search_depth, improving)) {
                // History pruning removes quiets that are both late and historically bad.
                continue;
            }

#if VALERAIN_CAPTURE_OBS
            int obs_capture_order = -1;
            int obs_see_value = 0;
            int obs_capture_hist = 0;
            if (capture_move) {
                obs_capture_order = capture_search_order++;
                obs_see_value = move_see_value;
                obs_capture_hist = capture_history_score;
                const int base_rank = lookup_capture_base_rank(
                    move, capture_moves, capture_base_ranks, capture_count
                );
                record_main_capture_try(
                    obs_capture_order, obs_see_value, obs_capture_hist, base_rank
                );
            }
#endif

            int move_extension = 0;
#if VALERAIN_ENABLE_SINGULAR_EXTENSION
            if (move == tt_move &&
                !checked &&
                !exclusion_search &&
                search_depth >= SINGULAR_MIN_DEPTH &&
                probe.hit &&
                tt_bound == memory::BOUND_LOWER &&
                probe.data.depth >= search_depth - SINGULAR_TT_DEPTH_MARGIN &&
                !is_mate_window(probed_tt_score)) {
#if VALERAIN_SEARCH_OBS
                ++search_obs.singular_candidates;
#endif
                const int singular_beta =
                    probed_tt_score - (SINGULAR_MARGIN_BASE + SINGULAR_MARGIN_PER_DEPTH * search_depth);
                const int singular_depth = std::max(1, search_depth / 2 - 1);
                const int singular_score = pvs(
                    pos,
                    singular_depth,
                    singular_beta - 1,
                    singular_beta,
                    ply,
                    false,
                    move
                );

                if (singular_score < singular_beta) {
                    move_extension = 1;
#if VALERAIN_SEARCH_OBS
                    ++search_obs.singular_extend1;
#endif
                    if (search_depth >= SINGULAR_DOUBLE_MIN_DEPTH &&
                        singular_score < singular_beta - (
                            SINGULAR_DOUBLE_MARGIN_BASE
                            + SINGULAR_DOUBLE_MARGIN_PER_DEPTH * search_depth
                        )) {
                        move_extension = 2;
#if VALERAIN_SEARCH_OBS
                        ++search_obs.singular_extend2;
#endif
                    }
                }
            }
#endif
            if (lmr_quiet_candidate || lmr_capture_candidate)
                gives_check = ensure_gives_check();
            const int continuation_score = quiet_move
                ? history_tables.continuation_value_fast(pos, move, prev_move)
                    + history_tables.continuation_value_fast(pos, move, prev2_move) / 2
                : 0;
            const int countermove_bonus = quiet_move
                ? history_tables.countermove_bonus_fast(pos, move, prev_move)
                : 0;
            const int move_see_bias_term = simple_capture
                ? history_tables.see_bias_value_fast(search_depth, move_see_value)
                : 0;
            LmrNodeContext lmr_node{};
            lmr_node.depth = search_depth;
            lmr_node.alpha = alpha;
            lmr_node.beta = beta;
            lmr_node.ply = ply;
            lmr_node.pv_node = pv_node;
            lmr_node.cut_node = !pv_node && !move_is_none(tt_move);
            lmr_node.all_node = !pv_node && move_is_none(tt_move);
            lmr_node.checked = checked;
            lmr_node.improving = improving;
            lmr_node.tt_move_present = !move_is_none(tt_move);
            lmr_node.tt_move_is_capture =
                !move_is_none(tt_move) && move_is_capture(tt_move);
            lmr_node.next_ply_cutoff_count = search_stack[ply + 1].cutoff_count;
            lmr_node.parent_reduction_fp = ply > 0 ? search_stack[ply - 1].reduction_fp : 0;

            LmrMoveContext lmr_move{};
            lmr_move.move = move;
            lmr_move.move_index = move_index;
            lmr_move.reduction_index = quiet_move ? move_index : (simple_capture_count - 1);
            lmr_move.is_tt_move = move == tt_move;
            lmr_move.quiet = quiet_move;
            lmr_move.capture = capture_move;
            lmr_move.simple_capture = simple_capture;
            lmr_move.gives_check = gives_check;
            lmr_move.recapture = is_recapture_move(move, ply);
            lmr_move.promotion = move_is_promotion(move);
            lmr_move.ordering_score = quiet_ordering_score;
            lmr_move.quiet_history_score = history_score;
            lmr_move.continuation_score = continuation_score;
            lmr_move.countermove_bonus = countermove_bonus;
            lmr_move.capture_history_score = capture_history_score;
            lmr_move.see_value = move_see_value;
            lmr_move.see_bias_term = move_see_bias_term;

            const LmrDecision lmr = decide_lmr(lmr_node, lmr_move);
            ss.current_move = move;
            ss.move_count = legal_count;
            ss.stat_score = lmr.stat_score;
            ss.reduction_fp = lmr.final_reduction_fp;

            StateInfo st;
            make_move(pos, move, mem.tables, st);
            memory::tt_prefetch(mem.tt, pos.key);
            move_stack[ply] = move;

            const int new_depth = search_depth - 1 + move_extension;
            int score = 0;
            int searched_depth = new_depth;
            if (!searched_first) {
                score = -pvs(pos, new_depth, -beta, -alpha, ply + 1, true);
                searched_first = true;
            } else {
                if (simple_capture) {
#if VALERAIN_CAPTURE_OBS
                    ++cap_obs.cap_lmr_late_simple_total;
                    if (search_depth >= 4 && simple_capture_count >= 3)
                        ++cap_obs.cap_lmr_eligible;
                    record_cap_lmr_considered(move_see_value, lmr.final_reduction);
#endif
                }

                if (lmr.eligible) {
                    const int reduced_depth = std::max(0, new_depth - lmr.final_reduction);
                    searched_depth = reduced_depth;
                    score = -pvs(pos, reduced_depth, -alpha - 1, -alpha, ply + 1, true);

                    if (score > alpha) {
                        const int research_depth =
                            lmr_research_depth(lmr, new_depth, score, alpha, alpha);
                        if (research_depth > reduced_depth) {
                            searched_depth = research_depth;
                            score = -pvs(
                                pos,
                                research_depth,
                                -alpha - 1,
                                -alpha,
                                ply + 1,
                                true
                            );
                        }
                    }
                } else {
                    score = -pvs(pos, new_depth, -alpha - 1, -alpha, ply + 1, true);
                }

#if VALERAIN_CAPTURE_OBS
                if (simple_capture && lmr.eligible && score > alpha)
                    record_cap_lmr_research(move_see_value, lmr.final_reduction);
#endif

                if (score > alpha && score < beta)
                    // A null-window fail-high inside the PV must be confirmed by
                    // a full-window re-search before the score is trusted.
                    score = -pvs(pos, searched_depth, -beta, -alpha, ply + 1, true);
            }

            unmake_move(pos, move, mem.tables, st);

            if (quiet_move)
                searched_quiets[searched_quiet_count++] = move;
#if VALERAIN_MOVEPICKER_OBS
            if (quiet_move)
                ++mp_obs.quiet_searched;
#endif
            if (capture_move) {
                searched_capture_see[searched_capture_count] = move_see_value;
                searched_captures[searched_capture_count++] = move;
            }

            if (score > alpha) {
                alpha = score;
                best_move = move;
                update_pv(ply, move);
                if (alpha >= beta) {
#if VALERAIN_MOVEPICKER_OBS
                    ++mp_obs.cutoffs_total;
                    ++mp_obs.cutoff_by_stage[static_cast<int>(stage_bucket)];
                    if (first_move_this && stage_bucket == MoveStageBucket::TT)
                        ++mp_obs.tt_first_cutoff;
                    if (first_good_capture_this)
                        ++mp_obs.first_good_capture_cutoff;
                    if (first_killer_this)
                        ++mp_obs.first_killer_cutoff;
                    if (first_quiet_this)
                        ++mp_obs.first_quiet_cutoff;
                    if (quiet_move && picker.last_quiet_in_skip_band())
                        ++mp_obs.late_quiet_fail_high;
                    if (quiet_move && picker.last_quiet_suppressed())
                        ++mp_obs.quiet_fail_high_after_skip_band;
#endif
#if VALERAIN_CAPTURE_OBS
                    if (capture_move)
                        record_main_capture_cutoff(
                            obs_capture_order, obs_see_value, obs_capture_hist
                        );
#endif
                    history_tables.reward_cutoff_fast(
                        pos,
                        move,
                        depth,
                        ply,
                        move_see_value,
                        prev_move,
                        prev2_move
                    );
                    if (capture_move)
                        history_tables.penalize_captures_fast(pos, searched_captures, searched_capture_count, move, depth);
                    else {
                        history_tables.penalize_quiets_fast(pos, searched_quiets, searched_quiet_count, move, depth);
                        history_tables.penalize_continuation_quiets_fast(
                            pos,
                            searched_quiets,
                            searched_quiet_count,
                            move,
                            prev_move,
                            prev2_move,
                            depth
                        );
                    }
                    history_tables.penalize_see_bias_captures_fast(
                        searched_captures,
                        searched_capture_see,
                        searched_capture_count,
                        move,
                        depth
                    );
                    ++ss.cutoff_count;
                    cutoff = true;
                    break;
                }
            }
        }

#if VALERAIN_MOVEPICKER_OBS
        mp_obs.quiet_generated += static_cast<u64>(picker.quiet_generated());
        mp_obs.quiet_scored += static_cast<u64>(picker.quiet_scored());
        mp_obs.quiet_skipped_by_mp += static_cast<u64>(picker.quiet_suppressed());
#endif

        if (legal_count == 0) {
            const int score = exclusion_search ? alpha : (checked ? (-VALUE_MATE + ply) : 0);
            if (!exclusion_search)
                save_tt(pos, search_depth, ply, score, raw_eval, 0, alpha0, beta, pv_node);
            return score;
        }

        if (!cutoff &&
            alpha > alpha0 &&
            best_move != 0) {
            const int hist_depth = std::max(1, search_depth - 1);
            if (move_is_capture(best_move)) {
                history_tables.bonus_capture_fast(pos, best_move, hist_depth);
                history_tables.penalize_captures_fast(pos, searched_captures, searched_capture_count, best_move, hist_depth);
            } else {
                history_tables.bonus_fast(pos, best_move, hist_depth);
                history_tables.penalize_quiets_fast(pos, searched_quiets, searched_quiet_count, best_move, hist_depth);
                history_tables.bonus_continuation_fast(pos, prev_move, best_move, hist_depth);
                history_tables.bonus_continuation_fast(pos, prev2_move, best_move, std::max(1, hist_depth / 2));
                history_tables.penalize_continuation_quiets_fast(
                    pos,
                    searched_quiets,
                    searched_quiet_count,
                    best_move,
                    prev_move,
                    prev2_move,
                    hist_depth
                );
            }
        }
        if (!cutoff && searched_capture_count > 0) {
            const int fail_depth = std::max(1, search_depth - 1);
            history_tables.penalize_see_bias_captures_fast(
                searched_captures,
                searched_capture_see,
                searched_capture_count,
                Move(0),
                fail_depth
            );
        }

        if (!exclusion_search &&
            !checked &&
            !cutoff &&
            best_move != 0 &&
            alpha > alpha0 &&
            alpha < beta &&
            !is_mate_window(alpha)) {
            update_correction_history(side, eval_info.keys, raw_eval, alpha, search_depth);
        }

        if (!exclusion_search)
            save_tt(pos, search_depth, ply, alpha, raw_eval, best_move, alpha0, beta, pv_node);
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
        update_seldepth(0);
        rep_keys[0] = root.key;

        MoveList list;
        GenInfo info;
        init_gen_info(info, root, mem);
        Move* rend = generate_pseudo_legal(root, mem, info, list.moves);
        list.size = static_cast<int>(rend - list.moves);

        if (limits.root_move_count > 0) {
            MoveList filtered;
            filtered.size = 0;
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
        const bool checked = in_check(root);
        const StaticEvalInfo eval_info = resolve_static_eval(root, probe, 0, checked, false);
        const int raw_eval = eval_info.raw;
        const int alpha0 = alpha;

        ScoredMoveList scored;
        score_moves(root, list, scored, root_hint, 0, depth);
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
            StateInfo st;
            make_move(root, move, mem.tables, st);
            memory::tt_prefetch(mem.tt, root.key);
            move_stack[0] = move;

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
            result.seldepth = seldepth;
            return result;
        }

        if (best_score == -VALUE_INF)
            best_score = alpha;

        if (!checked &&
            !is_mate_window(best_score) &&
            best_score > alpha0 &&
            best_score < beta) {
            update_correction_history(
                static_cast<Color>(root.side_to_move),
                eval_info.keys,
                raw_eval,
                best_score,
                depth
            );
        }

        result.score = best_score;
        result.nodes = nodes;
        result.seldepth = seldepth;
        save_tt(root, depth, 0, best_score, raw_eval, result.best_move, alpha0, beta, true);
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
    int cached_hashfull = 0;

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
            searcher.seldepth = 0;
            searcher.nmp_min_ply = 0;
            std::fill(std::begin(searcher.pv_length), std::end(searcher.pv_length), 0);
            std::fill(std::begin(searcher.move_stack), std::end(searcher.move_stack), Move(0));
            std::fill(
                std::begin(searcher.search_stack),
                std::end(searcher.search_stack),
                SearchStackEntry{}
            );
            std::fill(
                std::begin(searcher.static_eval_valid),
                std::end(searcher.static_eval_valid),
                false
            );

            current = searcher.search_root(keyed_root, depth, hint_move, alpha, beta);
            current.seldepth = searcher.seldepth;
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
            const u64 time_ms = static_cast<u64>(seconds * 1000.0);
            if (depth == 1 ||
                depth == max_depth ||
                (depth % HASHFULL_REPORT_PERIOD) == 0) {
                cached_hashfull = memory::tt_hashfull(mem.tt);
            }

            *out << "info depth " << depth
                 << " seldepth " << current.seldepth << ' ';
            append_uci_score(*out, current.score, root, searcher.use_nnue());
            *out << " nodes " << total_nodes
                 << " nps " << nps
                 << " hashfull " << cached_hashfull
                 << " time " << time_ms
                 << " pv";

            for (int i = 0; i < searcher.pv_length[0]; ++i)
                *out << ' ' << move_to_uci(searcher.pv_table[0][i]);

            *out << '\n';
            // if (searcher.use_nnue()) {
            //     *out << "info string winrate "
            //          << nnue::search_score_to_winrate(current.score, root)
            //          << '\n';
            // }
        }

        if (stopped_mid_depth)
            break;

        if (searcher.stop_after_completed_depth())
            break;
    }

    best.nodes = total_nodes;
#if VALERAIN_CAPTURE_OBS
    if (out)
        searcher.emit_capture_observation(*out);
#endif
#if VALERAIN_MOVEPICKER_OBS
    if (out)
        searcher.emit_movepicker_observation(*out);
#endif
#if VALERAIN_SEARCH_OBS
    if (out)
        searcher.emit_search_observation(*out);
#endif
    return best;
}

} // namespace valerain::search
