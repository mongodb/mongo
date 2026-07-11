// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace sbe {
class PlanStage;

/**
 * Utility struct to parameterize debug printing code. Passed as const reference through the SBE
 * stage printing functions.
 */
struct DebugPrintInfo {
    const bool printBytecode = false;
    int32_t callDepth = 0;
    // Some aggregation pipeline stages manifest as more than one SBE stage. Provide a 33% buffer
    // before capping the printing depth.
    const int32_t maxCallDepth = 4 * internalPipelineLengthLimit.loadRelaxed() / 3;
};

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
    std::string print(const PlanStage& s, DebugPrintInfo& debugPrintInfo);
    std::string print(const std::vector<Block>& blocks);

private:
    bool _colorConsole;

    void addIndent(int indent, std::string& s) {
        for (int i = 0; i < indent; ++i) {
            s.append("    ");
        }
    }
};

}  // namespace sbe
}  // namespace mongo
