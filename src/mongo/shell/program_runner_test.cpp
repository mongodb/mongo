// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/shell/program_runner.h"

#include "mongo/bson/json.h"
#include "mongo/db/wire_version.h"
#include "mongo/unittest/unittest.h"

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
    BSONObj args = BSON_ARRAY("cmd.exe" << "/C"
                                        << "echo"
                                        << "Hello, \"World\"!"
                                        << "Argument with spaces"
                                        << "!@#$%^&*\\");

    std::string expected =
        "\"Hello, \\\"World\\\"!\" \"Argument with spaces\" \"!@#$%^&*\\\\\"\r\n";

#else
    BSONObj args = BSON_ARRAY("echo" << "Hello, \"World\"!"
                                     << "Argument with spaces"
                                     << "!@#$%^&*\\");

    std::string expected = "Hello, \"World\"! Argument with spaces !@#$%^&*\\\n";
#endif

    BSONObj env{};

    // Create a program runner and start
    auto runner = registry->createProgramRunner(args, env, true, "");
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
