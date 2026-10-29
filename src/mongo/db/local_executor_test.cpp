// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/local_executor.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ProcessInterfaceStandaloneTest : public ScopedGlobalServiceContextForTest,
                                       public unittest::Test {
public:
    ProcessInterfaceStandaloneTest() : ScopedGlobalServiceContextForTest(true) {}

protected:
    void setUp() override {
        executor = createLocalExecutor(getServiceContext(), "test");
        executor->startup();
    }

    std::shared_ptr<executor::TaskExecutor> executor;
};

TEST_F(ProcessInterfaceStandaloneTest, StandaloneExecutorCanExecuteTasks) {
    int ran = false;
    ExecutorFuture<void>(executor).then([&]() { ran = true; }).get();

    ASSERT_TRUE(ran);
}

using ProcessInterfaceStandaloneTestDeathTest = ProcessInterfaceStandaloneTest;
DEATH_TEST_F(ProcessInterfaceStandaloneTestDeathTest,
             StandaloneExecutorThrowsOnRemoteExecution,
             "") {
    auto cr = executor::RemoteCommandRequest(
        HostAndPort("localhost"), DatabaseName::kAdmin, BSON("isMaster" << 1), BSONObj(), nullptr);

    std::ignore = executor->scheduleRemoteCommand(
        cr, [&](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {});
}

}  // namespace
}  // namespace mongo
