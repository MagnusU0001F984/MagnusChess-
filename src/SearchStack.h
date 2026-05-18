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

#include "Types.h"

/*
 * SearchStackEntry — 每個 ply 的搜尋狀態
 *
 * 在搜尋遞歸過程中，每個 ply 都需要保留一些狀態供後續 ply 使用。
 * 這些狀態透過 search_stack[] 陣列在 ply 之間傳遞，無需額外的
 * 函數參數或全局變量。
 *
 * 用途包括：
 *   - LMR 決策：stat_score 和 reduction_fp 被下一個 ply 讀取
 *   - 節點歷史信號：stat_score 用於 quiet_control_for_node 的歷史門檻計算
 *   - 截斷計數：cutoff_count 影響 LMR 減免量（連續截斷增加減免）
 *   - 將軍狀態：in_check 被多個剪枝函數讀取
 *   - TT 資訊：tt_hit 和 tt_pv 被空步剪枝使用
 */
namespace magnus::search {

struct SearchStackEntry {
    Move current_move = 0;          // 當前正在搜索的著法（供歷史更新使用）
    int static_eval = 0;            // 當前 ply 的靜態評估值
    int stat_score = 0;             // 綜合歷史統計分數（LMR 計算的輸出，供子節點參考）
    int reduction_fp = 0;           // LMR 的最終減免量（固定點格式，FP_ONE_PLY=1024）
                                    // 子節點用於 hindsight depth 調整
    int move_count = 0;             // 已搜索的合法著法數量
    int cutoff_count = 0;           // 截斷次數（連續截斷會增加後續著法的 LMR 減免量）
    bool in_check = false;          // 走子方是否被將軍
    bool tt_hit = false;            // 置換表是否命中（用於空步剪枝的閾值調整）
    bool tt_pv = false;             // 該 TT entry 是否來自 PV 節點
};

} // namespace magnus::search
