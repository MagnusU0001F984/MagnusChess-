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

#include <array>
#include <cstddef>

#include "Position.h"
#include "Search.h"
#include "Types.h"

/*
 * 時間管理模組 — Time Manager
 *
 * 將 UCI go 命令的原始時間參數轉換為標準化的 SearchLimits，
 * 並根據最近的搜尋歷史動態調整時間預算。
 *
 * 支援兩層時間控制：
 *   1. 硬限制 (hard_time_ms)：無論如何都不會超過的絕對上限
 *   2. 軟限制 (soft_time_ms)：時間管理演算法計算的最佳停止點
 *
 * 歷史自適應：
 *   TimeManager 記錄最近 64 次搜尋的耗時、深度、分數變化、
 *   最佳著法變更頻率等資訊，用於動態調整時間分配。
 *   若最佳著法頻繁變更，會分配更多時間；若局勢穩定，會提早停止。
 *
 * 時間管理由 Stockfish 風格的 SPSA 調參公式驅動（見 Search.cpp），
 *   綜合考慮 falling-eval、sigmoid 深度因子、最佳著法不穩定性。
 */
namespace magnus::timeman {

/*
 * GoParams — 從 UCI "go" 命令解析出的標準化時間參數
 *
 * 所有時間單位均為毫秒 (ms)。
 */
struct GoParams {
    int depth = 0;          // 固定深度限制（go depth N）
    u64 nodes = 0;          // 固定節點數限制（go nodes N）
    int movetime = 0;       // 固定時間限制（go movetime N）
    int wtime = 0;          // 白方剩餘時間（go wtime N）
    int btime = 0;          // 黑方剩餘時間（go btime N）
    int winc = 0;           // 白方增量（go winc N）
    int binc = 0;           // 黑方增量（go binc N）
    int movestogo = 0;      // 剩餘著法數（go movestogo N），0=未知
    bool ponder = false;    // 是否為沉思模式
    bool infinite = false;  // 是否為無限搜尋
};

/*
 * TimeManager — 時間管理器
 *
 * 擁有最近搜尋的歷史記錄，用於自適應時間分配。
 * 每個 UciSession 創建一個實例。
 */
class TimeManager {
public:
    TimeManager() = default;

    // new_game — 清空歷史記錄，開始新對局
    void new_game() noexcept;

    /*
     * build_limits — 從 GoParams 構建 SearchLimits
     *
     * 根據當前局面和時間參數計算軟/硬時間限制。
     * 若啟用時間管理，會根據歷史記錄調整預算。
     * 回傳 false 表示參數無效（缺少必要的時間控制）。
     */
    [[nodiscard]] bool build_limits(
        const Position& pos,
        const GoParams& params,
        search::SearchLimits& limits
    ) noexcept;

    /*
     * record_search — 記錄一次搜尋的結果
     *
     * 在搜尋完成後調用，將耗時、深度、分數等資訊存入歷史記錄。
     * 這些數據用於未來的時間預算調整。
     */
    void record_search(
        const Position& root,
        const search::SearchLimits& limits,
        const search::SearchResult& result,
        int elapsed_ms
    ) noexcept;

    // history_size — 當前歷史記錄的數量
    [[nodiscard]] std::size_t history_size() const noexcept;

private:
    // SearchRecord — 單次搜尋的歷史記錄
    struct SearchRecord {
        Color side = WHITE;             // 走子方
        int fullmove_number = 1;        // 完整回合數
        int soft_time_ms = 0;           // 軟時間限制
        int hard_time_ms = 0;           // 硬時間限制
        int elapsed_ms = 0;             // 實際耗時
        int depth = 0;                  // 完成的深度
        int score_cp = 0;               // 最終評分（cp）
        Move best_move = 0;             // 最佳著法
    };

    // HistoryStats — 從歷史記錄計算的統計數據
    struct HistoryStats {
        int samples = 0;                // 樣本數
        int avg_usage_pct = 100;        // 平均時間使用率（百分比）
        int avg_score_swing_cp = 0;     // 平均分數擺動（cp）
        int best_move_flip_pct = 50;    // 最佳著法變更率（百分比）
    };

    static constexpr std::size_t MAX_HISTORY = 64;  // 最大歷史記錄數

    std::array<SearchRecord, MAX_HISTORY> history_{}; // 環形歷史記錄緩衝區
    std::size_t history_size_ = 0;                     // 當前記錄數
    std::size_t next_index_ = 0;                       // 下一個寫入位置
    double original_time_adjust_ = -1.0;               // 原始時間調整因子（首次計算後鎖定）

    void push_record(const SearchRecord& record) noexcept;
    [[nodiscard]] HistoryStats collect_stats(Color side) const noexcept;
    [[nodiscard]] const SearchRecord& recent_record(std::size_t offset) const noexcept;
};

} // namespace magnus::timeman
