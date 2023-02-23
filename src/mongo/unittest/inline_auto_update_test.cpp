/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/unittest/inline_auto_update.h"
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
