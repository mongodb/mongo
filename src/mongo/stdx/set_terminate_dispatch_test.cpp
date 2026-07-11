// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/stdx/exception.h"
#include "mongo/util/exit_code.h"

#include <cstdlib>
#include <exception>
#include <iostream>

namespace {

namespace stdx = ::mongo::stdx;

void writeFeedbackAndCleanlyExit() {
    std::cout << "Entered terminate handler." << std::endl;
    exit(static_cast<int>(mongo::ExitCode::clean));
}

void testTerminateDispatch() {
    std::cout << "Setting terminate handler" << std::endl;
    stdx::set_terminate(writeFeedbackAndCleanlyExit);
    std::cout << "Calling terminate." << std::endl;
    std::terminate();
    exit(static_cast<int>(mongo::ExitCode::fail));
}
}  // namespace

int main() {
    testTerminateDispatch();
    return static_cast<int>(mongo::ExitCode::fail);
}
