/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/unittest/unittest_details.h"
#include "mongo/util/modules.h"

#include <gtest/gtest.h>

namespace mongo::unittest {

/**
 * Enhances test reporting, taking inspiration from doctest's test reporter.
 *  - Colorizes key details when --gtest_color configuration enables coloring.
 *  - Maintains progress indicator showing number of tests ran when writing to a TTY.
 *  - Suppresses output on passing tests unless configured to be verbose.
 *  - For failing tests, prints failure information with files + line numbers.
 *  - TODO(gtest): Prints a condensed summary of tests on conclusion.
 */
class EnhancedReporter : public testing::EmptyTestEventListener {
public:
    // Do not construct instances of these before mongo::unittest::initializeMain() is called.
    // The default values are runtime-dependent on system properties that are computed there.
    struct Options {
        bool showEachTest{false};
        bool useColor{details::gtestColorEnabled()};
        // Emit a spinner when we are writing to a TTY and colors are not explicitly disabled.
        bool useSpinner{details::stdoutIsTty() &&
                        (details::gtestColorEnabled() || details::gtestColorDefaulted())};
    };

    explicit EnhancedReporter(std::unique_ptr<testing::TestEventListener> defaultListener);

    EnhancedReporter(std::unique_ptr<testing::TestEventListener> defaultListener, Options options);

    ~EnhancedReporter() override;

    void OnTestProgramStart(const testing::UnitTest& unitTest) override;

    void OnTestIterationStart(const testing::UnitTest& unitTest, int iteration) override;

    void OnTestStart(const testing::TestInfo& testInfo) override;

    void OnTestPartResult(const testing::TestPartResult& res) override;

    void OnTestEnd(const testing::TestInfo& testInfo) override;

    void OnTestIterationEnd(const testing::UnitTest& unitTest, int iteration) override;

    void enable();
    void disable();

    /**
     * Dumps any buffered output. Safe to invoke from a signal handler. Logs that race with this
     * method may be lost as synchronization is best-effort.
     */
    void dumpBufferedOutputForSignalHandler();

private:
    class Impl;

    std::unique_ptr<Impl> _impl;
};

/**
 * Retrieves a pointer to the EnhancedReporter initialized during unittest set up. After
 * unittest setup, GoogleTest assumes ownership of the EnhancedReporter.
 */
extern EnhancedReporter* getGlobalEnhancedReporter();

}  // namespace mongo::unittest
