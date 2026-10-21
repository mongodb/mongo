// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/unittest_main_core.h"

#include <string>
#include <vector>

#include <fuzztest/init_fuzztest.h>
#include <gtest/gtest.h>

/**
 * Runner for fuzztests (mongo_cc_fuzztest targets).
 * Similar to the default google fuzztest runner, but performs mongo library initialization.
 */
int main(int argc, char** argv) {
    std::vector<std::string> argVec(argv, argv + argc);
    mongo::unittest::MainOptions options;

    // fuzztest runner uses signal handling, do not intercept signals
    options.startSignalProcessingThread = false;

    mongo::unittest::callInitGoogleTest(argVec);

    fuzztest::ParseAbslFlags(argc, argv);
    fuzztest::InitFuzzTest(&argc, &argv);

    // Pass only the program name to setupTestEnvironment, which will reject fuzztest args
    std::vector<std::string> initArgs;
    if (!argVec.empty()) {
        initArgs.push_back(argVec[0]);
    }
    mongo::unittest::setupTestEnvironment(options, initArgs);

    auto result = RUN_ALL_TESTS();
    mongo::unittest::finishTestEnvironment(options);

    return result;
}
