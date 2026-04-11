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

#include <string>

#include "Position.h"

namespace valerain::nnue {

/*
Thin wrapper around the embedded NNUE reader and evaluator. Search can switch
between HCE and NNUE through this interface without caring about file format
details or score conversion.
*/

struct WinRateParams {
    double a;
    double b;
};

struct WdlTriplet {
    int win;
    int draw;
    int loss;
};

// Network lifetime management.
bool load(const std::string& path);
void unload() noexcept;
bool loaded() noexcept;
const std::string& path() noexcept;
const std::string& description() noexcept;
const std::string& last_error() noexcept;

// Raw NNUE output and helper conversions back into engine-centric units.
int eval(const Position& pos) noexcept;
WinRateParams win_rate_params(const Position& pos) noexcept;
int to_cp(int v, const Position& pos) noexcept;
int win_rate_model(int v, const Position& pos) noexcept;
int search_score(int v, const Position& pos) noexcept;
int search_score_to_cp(int score, const Position& pos) noexcept;
int search_score_to_winrate(int score, const Position& pos) noexcept;
WdlTriplet search_score_to_wdl(int score, const Position& pos) noexcept;

// Incremental accumulator hooks wired into Position's board mutators.
void on_position_cleared(Position& pos) noexcept;
void on_piece_added(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept;
void on_piece_removed(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square sq
) noexcept;
void on_piece_moved(
    Position& pos,
    Color color,
    PieceType piece_type,
    Square from,
    Square to
) noexcept;

} // namespace valerain::nnue
