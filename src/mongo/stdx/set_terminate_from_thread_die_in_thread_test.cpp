/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/stdx/exception.h"

#include <stdlib.h>

#include <iostream>

#include "mongo/stdx/thread.h"
#include "mongo/util/exit_code.h"

namespace {

namespace stdx = ::mongo::stdx;

void writeFeedbackAndCleanlyExit() {
    std::cout << "Entered terminate handler." << std::endl;
    exit(static_cast<int>(mongo::ExitCode::clean));
}

void testTerminateDispatch() {
    std::cout << "Starting background thread (which will call `set_terminate`)." << std::endl;
    stdx::thread{[] {
        std::cout << "Setting terminate handler" << std::endl;
        stdx::set_terminate(writeFeedbackAndCleanlyExit);
    }}
        .join();
    std::cout << "Starting background thread (which will terminate)." << std::endl;
    stdx::thread{[] {
        std::cout << "Calling terminate from background thread." << std::endl;
        std::terminate();
    }}
        .join();
    exit(static_cast<int>(mongo::ExitCode::fail));
}
}  // namespace

int main() {
    testTerminateDispatch();
    return static_cast<int>(mongo::ExitCode::fail);
}
