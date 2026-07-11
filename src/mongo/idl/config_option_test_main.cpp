// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_main_core.h"

#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    mongo::unittest::MainProgress progress({}, std::vector<std::string>(argv, argv + argc));
    if (GTEST_FLAG_GET(internal_run_death_test).empty()) {
        auto& args = progress.args();
        args.insert(args.end(),
                    {
                        "--testConfigOpt2",
                        "true",
                        "--testConfigOpt8",
                        "8",
                        "--testConfigOpt12",
                        "command-line option",
                        "--testConfigOpt14",
                        "set14",
                    });
    }
    progress.initialize();
    return progress.test();
}
