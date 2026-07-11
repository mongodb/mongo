// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/pinned_connection_task_executor_factory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

namespace mongo {
namespace executor {
namespace {

class SearchTaskExecutorsTest : public unittest::Test {
public:
    void setUp() override {
        _serviceCtx = ServiceContext::make();
        _mongotExecutor = uassertStatusOK(getMongotTaskExecutor(_serviceCtx.get()));
        _mongotExecutor->startup();
        _searchIdxMgmtExecutor =
            uassertStatusOK(getSearchIndexManagementTaskExecutor(_serviceCtx.get()));
        _searchIdxMgmtExecutor->startup();
    }

    void tearDown() override {
        _serviceCtx.reset();
    }

    auto mongotExecutor() {
        return _mongotExecutor;
    }

    auto searchIndexMgmtExecutor() {
        return _searchIdxMgmtExecutor;
    }

    auto makeRCR() const {
        return RemoteCommandRequest(unittest::getFixtureConnectionString().getServers().front(),
                                    DatabaseName::kAdmin,
                                    BSON("isMaster" << 1),
                                    BSONObj(),
                                    nullptr);
    }

protected:
    ServiceContext::UniqueServiceContext _serviceCtx;
    std::shared_ptr<TaskExecutor> _mongotExecutor;
    std::shared_ptr<TaskExecutor> _searchIdxMgmtExecutor;
};

// Test fixture that constructs a pinned executor from the mongot executor,
// so that we can test running commands running over such a pinned executor.
class PinnedMongotTaskExecutorTest : public SearchTaskExecutorsTest {
public:
    void setUp() override {
        SearchTaskExecutorsTest::setUp();
        _pinnedExec = makePinnedConnectionTaskExecutor(_mongotExecutor);
    }

    auto pinnedExec() {
        return _pinnedExec;
    }

private:
    std::shared_ptr<TaskExecutor> _pinnedExec;
};

// Basic test that the search task executors are actually set up and work.
TEST_F(SearchTaskExecutorsTest, Basic) {
    auto doTestWithExecutor = [this](auto&& executor) {
        auto pf = makePromiseFuture<void>();

        ASSERT_OK(executor->scheduleRemoteCommand(
            makeRCR(), [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                pf.promise.setWith([&] { return args.response.status; });
            }));

        ASSERT_OK(pf.future.getNoThrow());
    };

    doTestWithExecutor(mongotExecutor());
    doTestWithExecutor(searchIndexMgmtExecutor());
}

TEST(SearchTaskExecutors, NotUsingIsNonFatal) {
    // Test purposefully makes a service context and immediately throws it away to ensure that we
    // can construct and destruct a service context (which is decorated with the search executors)
    // even if we never call startup().
    ServiceContext::make();
}

TEST_F(PinnedMongotTaskExecutorTest, RunSingleCommandOverPinnedConnection) {
    auto pf = makePromiseFuture<void>();

    ASSERT_OK(pinnedExec()->scheduleRemoteCommand(
        makeRCR(), [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        }));

    ASSERT_OK(pf.future.getNoThrow());
}

TEST_F(PinnedMongotTaskExecutorTest, RunMultipleCommandsOverPinnedConnection) {
    constexpr size_t numRequests = 2;
    std::vector<Future<void>> results;

    for (size_t i = 0; i < numRequests; ++i) {
        auto promise = std::make_shared<Promise<void>>(NonNullPromiseTag{});
        results.push_back(promise->getFuture());
        ASSERT_OK(pinnedExec()->scheduleRemoteCommand(
            makeRCR(),
            [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                p->setWith([&] { return args.response.status; });
            }));
    }

    ASSERT_EQ(numRequests, results.size());
    for (size_t i = 0; i < numRequests; ++i) {
        ASSERT_OK(results[i].getNoThrow());
    }
}

}  // namespace
}  // namespace executor
}  // namespace mongo
