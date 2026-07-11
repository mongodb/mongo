// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/unittest_main_core.h"
#include "mongo/util/quick_exit.h"

int main(int argc, char** argv) {
    std::vector<std::string> argVec(argv, argv + argc);
    mongo::unittest::MainProgress progress({.startSignalProcessingThread = false},
                                           std::move(argVec));
    progress.initialize();
    return progress.test();
}
