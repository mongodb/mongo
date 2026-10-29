// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/unittest/unittest_options.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::unittest {

struct SelectedTest {
    const char* suite;
    const char* test;
    bool keep;
};
std::string gtestFilterForSelection(const std::vector<SelectedTest>& selection);

struct FilterOptions {
    std::vector<std::string> suites;
    std::string filter;
    std::string fileNameFilter;
};

/** Config for `testMain`. */
struct [[MONGO_MOD_PUBLIC]] MainOptions {
    bool startSignalProcessingThread = true;
    bool suppressGlobalInitializers = false;
    bool enhancedReporter = false;
    bool showEachTest = false;

    /** Overrides if engaged */
    boost::optional<FilterOptions> filter;
};

/**
 * Tracks the state that must be carried among unit test program execution steps.
 */
class [[MONGO_MOD_PUBLIC]] MainProgress {
public:
    MainProgress(MainOptions options, std::vector<std::string> argVec)
        : _options{std::move(options)}, _argVec{std::move(argVec)} {}

    /**
     * Parses and removes flags relevant to test framework. Then performs
     * initialization, which includes initializing GoogleTest and calling Mongo's
     * global initializers.
     *
     * Call this at the top of main and then call `test` later.
     */
    void initialize();

    /**
     * Calls into GoogleTest's `RUN_ALL_TESTS` and performs post test cleanup.
     *
     * Call `initialize` before calling this.
     */
    int test();

    MainOptions& options() {
        return _options;
    }

    std::vector<std::string>& args() {
        return _argVec;
    }

private:
    /**
     * Parses, validates, and applies unit test options. Must run before
     * testing::InitGoogleTest so that `--filter`, `--suites`, and `--fileNameFilter`
     * work correctly. Returns an exit code if the program should exit immediately.
     */
    boost::optional<ExitCode> _parseAndAcceptOptions();

    MainOptions _options;
    std::vector<std::string> _argVec;
};

}  // namespace mongo::unittest
