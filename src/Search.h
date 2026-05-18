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

/*
 * 搜尋層公開介面 — MagnusChess 的核心搜尋引擎。
 *
 * 本標頭對外暴露兩個入口點：
 *   1. iterative_deepening() — 從根節點啟動反覆加深 PVS 搜尋，供 bench 與 UCI 迴圈調用
 *   2. move_to_uci() — 將內部 16 位元著法轉換為 UCI 座標字串
 *
 * 內部實作採用經典的迭代加深 + 主變例搜尋 (PVS) 架構，
 * 搭配置換表導向排序、空步剪枝、奇異延伸、機率截斷、
 * 以及一組低開銷的啟發式剪枝技術。
 *
 * 所有搜尋狀態封裝在 Search.cpp 的匿名命名空間中，
 * 對外部完全透明。
 */
namespace magnus::search {

/*
 * 搜尋層全局常數：
 *   MAX_PLY            — 最大搜索深度（半步數），用於所有棧陣列的固定尺寸
 *   MAX_GAME_HISTORY   — 最大對局歷史記錄數，用於重複局面檢測
 */
constexpr int MAX_PLY = 128;
constexpr int MAX_GAME_HISTORY = 128;

/*
 * SearchLimits — 搜尋限制參數集合
 *
 * 由 bench 命令或 UCI "go" 命令建構，控制搜尋的終止條件。
 * 支援多種限制類型：深度、節點數、軟/硬時間限制、無限搜尋、沉思模式。
 *
 * 多線程支援：
 *   shared_nodes  — 跨線程共享的節點計數器（Lazy SMP 用）
 *   stop / external_stop — 合作式停止信號（原子布林）
 *   pondering / ponder_time_offset_ms — 沉思模式時間追蹤
 *   thread_id / thread_count — 線程標識與總數
 */
struct SearchLimits {
    // --- 搜尋深度與節點限制 ---
    int depth = MAX_PLY;                // 最大搜索深度（ply），預設為無限
    u64 node_limit = 0;                 // 最大節點數限制，0 表示無限制

    // --- 時間控制 ---
    int soft_time_ms = 0;               // 軟時間限制（毫秒），用於時間管理
    int hard_time_ms = 0;               // 硬時間限制（毫秒），強制停止
    bool ponder = false;                // 是否為沉思模式（對手時間內搜尋）
    bool infinite = false;              // 是否為無限搜尋模式
    bool use_time_management = false;   // 是否啟用 Stockfish 風格時間管理

    // --- 引擎選項 ---
    int contempt = 0;                   // 輕視值：正值傾向避免和棋，負值傾向接受和棋
    bool use_nnue = false;              // 是否使用 NNUE 神經網路評估

    // --- 對局歷史（供重複局面檢測）---
    Key game_history_keys[MAX_GAME_HISTORY]{}; // 歷史局面的 Zobrist 鍵值
    int game_history_count = 0;                // 已記錄的歷史局面數量

    // --- 根節點著法限制（UCI searchmoves）---
    Move root_moves[256]{};             // 限制搜尋的根節點著法列表
    int root_move_count = 0;            // 限制的著法數量，0 = 全部合法著法

    // --- 多線程同步 ---
    std::atomic<bool>* stop = nullptr;                      // 本線程的停止信號
    const std::atomic<bool>* external_stop = nullptr;       // 外部停止信號（跨線程）
    const std::atomic<bool>* pondering = nullptr;           // 沉思模式啟用標誌
    const std::atomic<int>* ponder_time_offset_ms = nullptr;// 沉思時間偏移量
    std::atomic<u64>* shared_nodes = nullptr;               // 共享節點計數器

    // --- 線程資訊 ---
    int thread_id = 0;                  // 本線程的 ID（0 = 主線程）
    int thread_count = 1;               // 總線程數
    bool report_info = true;            // 是否輸出 UCI info 資訊（輔助線程設為 false）
};

/*
 * SearchResult — 搜尋結果
 *
 * 反覆加深完成後回傳的結構，包含最佳著法、評分、
 * 以及整體搜尋統計（節點數、深度、選擇性深度）。
 */
struct SearchResult {
    Move best_move = 0;                 // 搜尋找到的最佳著法（0 = 無合法著法）
    Score score = 0;                    // 最佳著法的評分（cp 單位，從根節點視角）
    u64 nodes = 0;                      // 搜尋探索的總節點數
    int depth = 0;                      // 完成的搜索深度（ply）
    int seldepth = 0;                   // 選擇性深度：最深的分支實際搜索深度
};

/*
 * move_to_uci — 將內部 16 位元著法格式轉換為 UCI 座標表示法
 *
 * 參數：
 *   m — 內部 Move 格式（16 位元，包含起點/終點/升變資訊）
 *
 * 回傳：
 *   格式為 "e2e4" 或 "e7e8q"（升變）的 UCI 字串
 *   若 m 為空著法（0），回傳 "0000"
 */
[[nodiscard]] std::string move_to_uci(Move m);

/*
 * iterative_deepening — 搜尋入口點
 *
 * 從根節點位置啟動完整的反覆加深搜尋。
 * 根據 thread_count 自動選擇單線程或 Lazy SMP 並行路徑。
 *
 * 參數：
 *   root   — 根節點局面（const 參考，內部會複製）
 *   mem    — 共享記憶體（置換表、兵表、材料表、攻擊表）
 *   limits — 搜尋限制參數
 *   out    — 可選的輸出串流，用於 UCI info 輸出（nullptr = 不輸出）
 *
 * 回傳：
 *   SearchResult — 最佳著法、評分、搜尋統計
 */
[[nodiscard]] SearchResult iterative_deepening(
    const Position& root,
    memory::Memory& mem,
    const SearchLimits& limits,
    std::ostream* out = nullptr
);

} // namespace magnus::search
