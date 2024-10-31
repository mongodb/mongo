/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"

#include "mongo/db/wire_version.h"
#include "mongo/shell/program_runner.h"

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::shell_utils {

class ProgramRunnerTestFixture : public unittest::Test {

protected:
    ProgramRunnerTestFixture() {
        auto serviceContext = ServiceContext::make();
        WireSpec::getWireSpec(serviceContext.get()).initialize(WireSpec::Specification{});
        setGlobalServiceContext(std::move(serviceContext));

        ProgramRegistry::create(getGlobalServiceContext());
        registry = ProgramRegistry::get(getGlobalServiceContext());
    }

    ProgramRegistry* registry;
};


TEST_F(ProgramRunnerTestFixture, ArgumentEscaping) {
#ifdef _WIN32
    BSONObj args = BSON_ARRAY("cmd.exe"
                              << "/C"
                              << "echo"
                              << "Hello, \"World\"!"
                              << "Argument with spaces"
                              << "!@#$%^&*\\");

    std::string expected =
        "\"Hello, \\\"World\\\"!\" \"Argument with spaces\" \"!@#$%^&*\\\\\"\r\n";

#else
    BSONObj args = BSON_ARRAY("echo"
                              << "Hello, \"World\"!"
                              << "Argument with spaces"
                              << "!@#$%^&*\\");

    std::string expected = "Hello, \"World\"! Argument with spaces !@#$%^&*\\\n";
#endif

    BSONObj env{};

    // Create a program runner and start
    auto runner = registry->createProgramRunner(args, env, true);
    runner.start(true);

    // Wait for PID so we can read output
    stdx::thread t(runner, registry->getProgramOutputMultiplexer(), true);
    registry->registerReaderThread(runner.pid(), std::move(t));
    int exit_code = -123456;
    registry->waitForPid(runner.pid(), true, &exit_code);

    // Read output
    auto programOutputLogger =
        ProgramRegistry::get(getGlobalServiceContext())->getProgramOutputMultiplexer();
    std::string programLog = programOutputLogger->str();

    ASSERT_STRING_CONTAINS(programLog, expected);

    ASSERT(!exit_code);
}
}  // namespace mongo::shell_utils
