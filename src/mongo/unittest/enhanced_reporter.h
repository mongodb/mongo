// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        // Colorize when we are writing to a TTY and colors are not explicitly disabled.
        bool useColor{details::stdoutIsTty() &&
                      (details::gtestColorEnabled() || details::gtestColorDefaulted())};
        // The spinner applies some styling that use ANSI codes, so we should follow GTest's
        // detection of determining whether ANSI codes are tolerable at the output destination.
        bool useSpinner{useColor};
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
