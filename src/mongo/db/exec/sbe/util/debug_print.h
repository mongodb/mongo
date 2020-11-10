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

#pragma once

#include <string>
#include <vector>

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
class PlanStage;

class DebugPrinter {
public:
    struct Block {
        enum Command {
            cmdIncIndent,
            cmdDecIndent,
            cmdNone,
            cmdNoneNoSpace,
            cmdNewLine,
            cmdColorRed,
            cmdColorGreen,
            cmdColorBlue,
            cmdColorCyan,
            cmdColorYellow,
            cmdColorNone
        };

        Command cmd;
        std::string str;

        Block(std::string_view s) : cmd(cmdNone), str(s) {}

        Block(Command c, std::string_view s) : cmd(c), str(s) {}

        Block(Command c) : cmd(c) {}
    };

    DebugPrinter(bool colorConsole = false) : _colorConsole(colorConsole) {}

    static void addKeyword(std::vector<Block>& ret, std::string_view k) {
        ret.emplace_back(Block::cmdColorCyan);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, k});
        ret.emplace_back(Block::cmdColorNone);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, " "});
    }

    static void addIdentifier(std::vector<Block>& ret, value::SlotId slot) {
        std::string name{str::stream() << "s" << slot};
        ret.emplace_back(Block::cmdColorGreen);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, name});
        ret.emplace_back(Block::cmdColorNone);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, " "});
    }

    static void addIdentifier(std::vector<Block>& ret, FrameId frameId, value::SlotId slot) {
        std::string name{str::stream() << "l" << frameId << "." << slot};
        ret.emplace_back(Block::cmdColorGreen);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, name});
        ret.emplace_back(Block::cmdColorNone);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, " "});
    }

    static void addIdentifier(std::vector<Block>& ret, std::string_view k) {
        ret.emplace_back(Block::cmdColorGreen);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, k});
        ret.emplace_back(Block::cmdColorNone);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, " "});
    }

    static void addSpoolIdentifier(std::vector<Block>& ret, SpoolId spool) {
        std::string name{str::stream() << "sp" << spool};
        ret.emplace_back(Block::cmdColorGreen);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, name});
        ret.emplace_back(Block::cmdColorNone);
        ret.emplace_back(Block{Block::cmdNoneNoSpace, " "});
    }

    static void addNewLine(std::vector<Block>& ret) {
        ret.emplace_back(Block::cmdNewLine);
    }

    static void addBlocks(std::vector<Block>& ret, std::vector<Block> blocks) {
        ret.insert(ret.end(),
                   std::make_move_iterator(blocks.begin()),
                   std::make_move_iterator(blocks.end()));
    }
    std::string print(PlanStage* s);
    std::string print(const std::vector<Block>& blocks);

private:
    bool _colorConsole;

    void addIndent(int ident, std::string& s) {
        for (int i = 0; i < ident; ++i) {
            s.append("    ");
        }
    }
};
}  // namespace sbe
}  // namespace mongo
