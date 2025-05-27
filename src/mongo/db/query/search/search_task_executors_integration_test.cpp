/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/service_context.h"
#include "mongo/executor/pinned_connection_task_executor_factory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

#include "search_task_executors.h"

namespace mongo {
namespace executor {
namespace {

class SearchTaskExecutorsTest : public unittest::Test {
public:
    void setUp() override {
        _serviceCtx = ServiceContext::make();
        _mongotExecutor = getMongotTaskExecutor(_serviceCtx.get());
        _mongotExecutor->startup();
        _searchIdxMgmtExecutor = getSearchIndexManagementTaskExecutor(_serviceCtx.get());
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
