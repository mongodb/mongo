// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/util/debug_print.h"

#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo::sbe {
namespace {
using Block = DebugPrinter::Block;

template <typename... Args>
std::string render(Args&&... args) {
    std::vector<Block> blocks;
    (blocks.push_back(Block{args}), ...);
    return DebugPrinter{}.print(blocks);
}

TEST(DebugPrintTest, JoinsWordsWithSingleSpaces) {
    ASSERT_EQ(render("a", "b", "c"), "a b c");
}

TEST(DebugPrintTest, LeadingBacktickRemovesPrecedingSpace) {
    ASSERT_EQ(render("[", "a", "`]"), "[ a]");
    ASSERT_EQ(render("f", "`(`", "x", "`)"), "f(x)");
}

TEST(DebugPrintTest, LeadingBacktickKeepsIndentation) {
    ASSERT_EQ(render("if", Block{Block::cmdIncIndent}, "a", Block{Block::cmdDecIndent}, "`)"),
              "if\n    a\n)");
}

TEST(DebugPrintTest, NoTrailingSpaceOnLines) {
    ASSERT_EQ(render("a", Block{Block::cmdNewLine}, "b"), "a\nb");
    ASSERT_EQ(render("a", "b"), "a b");
}

TEST(DebugPrintTest, NoBlankLines) {
    ASSERT_EQ(render("a",
                     Block{Block::cmdIncIndent},
                     "b",
                     Block{Block::cmdDecIndent},
                     Block{Block::cmdNewLine},
                     "c"),
              "a\n    b\nc");
}

TEST(DebugPrintTest, AdjacentDecrementsCollapse) {
    ASSERT_EQ(render("a",
                     Block{Block::cmdIncIndent},
                     "b",
                     Block{Block::cmdIncIndent},
                     "c",
                     Block{Block::cmdDecIndent},
                     Block{Block::cmdDecIndent},
                     "d"),
              "a\n    b\n        c\nd");
}

TEST(DebugPrintTest, NoTrailingWhitespaceAtEnd) {
    ASSERT_EQ(render("a", Block{Block::cmdNewLine}), "a");
    ASSERT_EQ(render("a", Block{Block::cmdIncIndent}, "b", Block{Block::cmdDecIndent}), "a\n    b");
}
}  // namespace
}  // namespace mongo::sbe
