/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/sbe/util/debug_print.h"

#include "mongo/db/exec/sbe/stages/stages.h"

#include <cstddef>
#include <memory>

namespace mongo {
namespace sbe {
std::string DebugPrinter::print(const std::vector<Block>& blocks) {
    std::string ret;
    int ident = 0;
    size_t blockIndex = 0;
    for (auto& b : blocks) {
        bool addSpace = true;
        switch (b.cmd) {
            case Block::cmdIncIndent:
                ++ident;
                ret.append("\n");
                addIndent(ident, ret);
                break;
            case Block::cmdDecIndent:
                --ident;
                // Avoid unnecessary whitespace if there are multiple adjacent "decrement indent"
                // tokens.
                if (blockIndex >= (blocks.size() - 1) ||
                    blocks[blockIndex + 1].cmd != Block::cmdDecIndent) {
                    ret.append("\n");
                    addIndent(ident, ret);
                }
                break;
            case Block::cmdNewLine:
                ret.append("\n");
                addIndent(ident, ret);
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

        StringData sv(b.str);
        if (!sv.empty()) {
            if (*sv.begin() == '`') {
                sv = sv.substr(1);
                if (!ret.empty() && ret.back() == ' ') {
                    ret.resize(ret.size() - 1, 0);
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

    return ret;
}

std::string DebugPrinter::print(const PlanStage& s) {
    return print(s.debugPrint());
}
}  // namespace sbe
}  // namespace mongo
