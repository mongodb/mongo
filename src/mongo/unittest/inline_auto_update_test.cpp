// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/unittest/unittest.h"

namespace mongo::unittest {
namespace {
TEST(AutoUpdateAssertion, DiffTest) {
    const std::vector<std::string> expected{"line 1\n",
                                            "line 2\n",
                                            "line 3\n",
                                            "line 5\n",
                                            "line 7\n",
                                            "line 8\n",
                                            "line 10\n",
                                            "line 11\n"};
    const std::vector<std::string> actual{"line 2\n",
                                          "line 4\n",
                                          "line 5\n",
                                          "line 6\n",
                                          "line 8\n",
                                          "line 10\n",
                                          "line 11\n",
                                          "line 12\n",
                                          "line 13\n"};

    std::string diffStr;

    {
        std::ostringstream os;
        outputDiff(os, expected, actual, 0);
        diffStr = os.str();
    }
    ASSERT_STR_EQ_AUTO(  // NOLINT (test auto-update)
        "L0: -line 1\n"
        "L1: +line 4\n"
        "L2: -line 3\n"
        "L3: +line 6\n"
        "L4: -line 7\n"
        "L7: +line 12\n"
        "L8: +line 13\n",
        diffStr);

    {
        std::ostringstream os;
        outputDiff(os, {}, {"line1\n", "line2\n", "line3\n"}, 10);
        diffStr = os.str();
    }
    ASSERT_STR_EQ_AUTO(  // NOLINT (test auto-update)
        "L10: +line1\n"
        "L11: +line2\n"
        "L12: +line3\n",
        diffStr);

    {
        std::ostringstream os;
        outputDiff(os, {"line1\n", "line2\n", "line3\n"}, {}, 7);
        diffStr = os.str();
    }
    ASSERT_STR_EQ_AUTO(  // NOLINT (test auto-update)
        "L7: -line1\n"
        "L8: -line2\n"
        "L9: -line3\n",
        diffStr);

    {
        std::ostringstream os;
        outputDiff(os, {}, {}, 10);
        diffStr = os.str();
    }
    ASSERT(diffStr.empty());
}

TEST(AutoUpdateAssertion, TestTest) {
    std::string oneLine = "hello";
    ASSERT_STR_EQ_AUTO(  // NOLINT
        "hello",         // NOLINT (test auto-update)
        oneLine);

    std::string multiLineNoNewline = "hello\nworld";
    ASSERT_STR_EQ_AUTO(  // NOLINT
        "hello\n"
        "world",
        multiLineNoNewline);

    std::string multiLineNewlineEnd = "hello\nworld\n";
    ASSERT_STR_EQ_AUTO(  // NOLINT
        "hello\n"
        "world\n",
        multiLineNewlineEnd);
}
}  // namespace
}  // namespace mongo::unittest
