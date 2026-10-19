// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/util/debug_print.h"

#include "mongo/db/exec/sbe/stages/stages.h"

#include <cstddef>
#include <string_view>

namespace mongo {
namespace sbe {
std::string DebugPrinter::print(const std::vector<Block>& blocks) {
    std::string ret;
    int ident = 0;
    size_t blockIndex = 0;
    size_t contentStart = 0;
    auto trimTrailingSpaces = [&] {
        while (ret.size() > 0 && ret.back() == ' ') {
            ret.pop_back();
        }
    };
    auto startLine = [&] {
        trimTrailingSpaces();
        if (ret.empty() || ret.back() != '\n') {
            ret.append("\n");
        }
        addIndent(ident, ret);
        contentStart = ret.size();
    };
    for (auto& b : blocks) {
        bool addSpace = true;
        switch (b.cmd) {
            case Block::cmdIncIndent:
                ++ident;
                startLine();
                break;
            case Block::cmdDecIndent:
                --ident;
                // Avoid unnecessary whitespace if there are multiple adjacent "decrement indent"
                // tokens.
                if (blockIndex >= (blocks.size() - 1) ||
                    blocks[blockIndex + 1].cmd != Block::cmdDecIndent) {
                    startLine();
                }
                break;
            case Block::cmdNewLine:
                startLine();
                break;
            case Block::cmdNone:
                break;
            case Block::cmdNoneNoSpace:
                addSpace = false;
                break;
            case Block::cmdColorRed:
                if (_colorConsole) {
                    ret.append("\033[0;31m");
                }
                break;
            case Block::cmdColorGreen:
                if (_colorConsole) {
                    ret.append("\033[0;32m");
                }
                break;
            case Block::cmdColorBlue:
                if (_colorConsole) {
                    ret.append("\033[0;34m");
                }
                break;
            case Block::cmdColorCyan:
                if (_colorConsole) {
                    ret.append("\033[0;36m");
                }
                break;
            case Block::cmdColorYellow:
                if (_colorConsole) {
                    ret.append("\033[0;33m");
                }
                break;
            case Block::cmdColorNone:
                if (_colorConsole) {
                    ret.append("\033[0m");
                }
                break;
        }

        std::string_view sv(b.str);
        if (!sv.empty()) {
            if (*sv.begin() == '`') {
                sv = sv.substr(1);
                if (ret.size() > contentStart && ret.back() == ' ') {
                    ret.pop_back();
                }
            }
            if (!sv.empty() && *(sv.end() - 1) == '`') {
                sv = sv.substr(0, sv.size() - 1);
                addSpace = false;
            }
            if (!sv.empty()) {
                ret.append(sv.begin(), sv.end());
                if (addSpace) {
                    ret.append(" ");
                }
            }
        }
        ++blockIndex;
    }

    while (!ret.empty() && (ret.back() == ' ' || ret.back() == '\n')) {
        ret.pop_back();
    }
    return ret;
}

std::string DebugPrinter::print(const PlanStage& s, DebugPrintInfo& debugPrintInfo) {
    return print(s.debugPrint(debugPrintInfo));
}
}  // namespace sbe
}  // namespace mongo
