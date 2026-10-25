// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <vector>
namespace dust {
// Native replacement for CHECKERS.DLL's checkmove plugin. Board/move strings
// use the unchanged original checkers scripts' contract.
std::string checkersMove(const std::string &board, int depth, int side);
// Legal input paths for deterministic gameplay fixtures; no state is mutated.
std::vector<std::string> checkersLegalMoves(const std::string &board, int side);
} // namespace dust
