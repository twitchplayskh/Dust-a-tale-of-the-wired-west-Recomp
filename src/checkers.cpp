// SPDX-License-Identifier: GPL-3.0-only
// Adapted from pinned dreamREfactory runtime/checkers.ts (GPL-3.0), whose
// search/order/evaluation are checked against CHECKERS.DLL emulator vectors.
#include "checkers.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace dust {
namespace {
using Board = std::array<int8_t, 64>;
struct Hop {
    int row, col, code;
};
std::optional<Board> parseBoard(const std::string &text) {
    Board b{};
    std::istringstream input(text);
    int value;
    for (auto &cell : b) {
        if (!(input >> value) || value < -2 || value > 2)
            return {};
        cell = int8_t(value);
    }
    std::string tail;
    if (input >> tail)
        return {};
    return b;
}
std::string encode(const std::vector<Hop> &chain) {
    std::string answer;
    for (const auto &hop : chain) {
        answer += char('0' + hop.row);
        answer += char('0' + hop.col);
        answer += char('0' + hop.code);
        answer += ',';
    }
    return answer;
}
constexpr int dr[]{0, -2, 2, 2, -2, -1, 1, 1, -1}, dc[]{0, 2, 2, -2, -2, 1, 1, -1, -1};
bool on(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}
bool legal(const Board &b, int row, int col, int code, int side) {
    const int piece = b[row * 8 + col], r = row + dr[code], c = col + dc[code];
    if (!piece || (piece < 0) != (side != 0) || !on(r, c) || b[r * 8 + c])
        return false;
    if (std::abs(piece) != 2 && (side == 0 ? dr[code] < 0 : dr[code] > 0))
        return false;
    return code > 4 || piece * b[(row + dr[code] / 2) * 8 + col + dc[code] / 2] < 0;
}
std::vector<Hop> generate(const Board &b, int side) {
    for (int first : {1, 5}) {
        std::vector<Hop> moves;
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c)
                for (int k = first; k < first + 4; ++k)
                    if (legal(b, r, c, k, side))
                        moves.push_back({r, c, k});
        if (!moves.empty())
            return moves;
    }
    return {};
}
std::vector<Hop> play(Board &b, Hop hop, int side) {
    std::vector<Hop> chain;
    for (;;) {
        chain.push_back(hop);
        const int r = hop.row + dr[hop.code], c = hop.col + dc[hop.code];
        int piece = b[hop.row * 8 + hop.col];
        b[hop.row * 8 + hop.col] = 0;
        if (side == 0 ? r == 7 : r == 0)
            piece = side == 0 ? 2 : -2;
        b[r * 8 + c] = int8_t(piece);
        if (hop.code > 4)
            break;
        b[((hop.row + r) / 2) * 8 + (hop.col + c) / 2] = 0;
        int next = 1;
        for (; next <= 4; ++next)
            if (legal(b, r, c, next, side))
                break;
        if (next > 4)
            break;
        hop = {r, c, next};
    }
    return chain;
}
int evaluate(const Board &b) {
    int score = 0;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            const int piece = b[r * 8 + c];
            if (!piece)
                continue;
            score += 100 * piece;
            for (int k = 5; k <= 8; ++k)
                if (on(r + dr[k], c + dc[k])) {
                    const int neighbor = b[(r + dr[k]) * 8 + c + dc[k]];
                    if (neighbor * piece < 0)
                        score += std::abs(neighbor);
                    else if (!neighbor)
                        score += piece > 0 ? -1 : 1;
                }
        }
    return score;
}
struct Result {
    std::optional<Hop> best;
    int score;
};
Result search(const Board &b, int depth, int side) {
    const auto moves = generate(b, side);
    if (moves.empty() || depth <= 0)
        return {moves.empty() ? std::nullopt : std::optional<Hop>(moves.front()), evaluate(b)};
    Result result{std::nullopt, side == 0 ? -999 : 999};
    for (const auto &move : moves) {
        auto child = b;
        play(child, move, side);
        const int score = search(child, depth - 1, 1 - side).score;
        if (side == 0 ? score > result.score : score < result.score)
            result = {move, score};
    }
    return result;
}
} // namespace
std::string checkersMove(const std::string &text, int depth, int side) {
    const auto parsed = parseBoard(text);
    if (!parsed)
        return {};
    auto b = *parsed;
    side = side == 0 ? 0 : 1;
    depth = std::max(0, int(int16_t(depth)));
    if (depth > 6)
        throw std::runtime_error("checkers lookahead exceeds native safety bound");
    const auto moves = generate(b, side);
    if (moves.empty())
        return {};
    // The DLL leaves stale stack data if no candidate beats +/-999. Choose
    // its ordinary depth-zero first legal move in that undefined case.
    const auto best = search(b, depth, side).best.value_or(moves.front());
    return encode(play(b, best, side));
}
std::vector<std::string> checkersLegalMoves(const std::string &text, int side) {
    const auto parsed = parseBoard(text);
    if (!parsed)
        return {};
    side = side == 0 ? 0 : 1;
    std::vector<std::string> answers;
    for (const auto &move : generate(*parsed, side)) {
        auto b = *parsed;
        answers.push_back(encode(play(b, move, side)));
    }
    return answers;
}
} // namespace dust
