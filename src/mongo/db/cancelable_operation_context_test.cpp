/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace {

class CancelableOperationContextTest : public unittest::Test {
public:
    void setUp() override {
        ThreadPool::Options options;
        options.poolName = "CancelableOperationContextTest";
        options.minThreads = 1;
        options.maxThreads = 1;

        _threadPool = std::make_shared<ThreadPool>(std::move(options));
        _threadPool->startup();
    }

    void tearDown() override {
        _threadPool->shutdown();
        _threadPool->join();
        _threadPool.reset();
    }

    ExecutorPtr executor() {
        return _threadPool;
    }

    void waitForAllEarlierTasksToComplete() {
        _threadPool->waitForIdle();
    }

    void shutDownExecutor() {
        _threadPool->shutdown();
    }

private:
    std::shared_ptr<ThreadPool> _threadPool;
};

TEST_F(CancelableOperationContextTest, ActsAsNormalOperationContext) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), CancellationToken::uncancelable(), executor()};

    ASSERT_EQ(opCtx->getClient(), client.get());
    ASSERT_EQ(opCtx.get()->getClient(), client.get());

    // The CancellationSource underlying the OperationContext* is unassociated with the one supplied
    // to the CancelableOperationContext constructor.
    ASSERT_TRUE(opCtx->getCancellationToken().isCancelable());
}

TEST_F(CancelableOperationContextTest, KilledWhenCancellationSourceIsCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    cancelSource.cancel();
    waitForAllEarlierTasksToComplete();
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(CancelableOperationContextTest,
       KilledUponConstructionWhenCancellationSourceAlreadyCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    shutDownExecutor();
    CancellationSource cancelSource;
    cancelSource.cancel();

    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(CancelableOperationContextTest, SafeWhenCancellationSourceIsCanceledUnderClientMutex) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    {
        // Holding the Client mutex while canceling the CancellationSource won't lead to
        // self-deadlock.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        cancelSource.cancel();
    }
    waitForAllEarlierTasksToComplete();
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(CancelableOperationContextTest, SafeWhenDestructedBeforeCancellationSourceIsCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    boost::optional<CancelableOperationContext> opCtx;
    opCtx.emplace(client->makeOperationContext(), cancelSource.token(), executor());

    opCtx.reset();
    cancelSource.cancel();
}

TEST_F(CancelableOperationContextTest, NotKilledWhenCancellationSourceIsDestructed) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    boost::optional<CancellationSource> cancelSource;
    cancelSource.emplace();
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource->token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    cancelSource.reset();
    ASSERT_OK(opCtx->checkForInterruptNoAssert());
}

TEST_F(CancelableOperationContextTest,
       NotKilledWhenCancellationSourceIsCanceledAndTaskExecutorAlreadyShutDown) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    shutDownExecutor();
    cancelSource.cancel();
    ASSERT_OK(opCtx->checkForInterruptNoAssert());
}

TEST_F(CancelableOperationContextTest, SafeWhenOperationContextOwnCancellationTokenIsUsed) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    auto opCtx = client->makeOperationContext();
    auto cancelToken = opCtx->getCancellationToken();
    auto cancelableOpCtx = CancelableOperationContext{std::move(opCtx), cancelToken, executor()};

    ASSERT_OK(cancelableOpCtx->checkForInterruptNoAssert());

    auto expectedErrorCode = ErrorCodes::Error(5510299);
    {
        // Acquiring the Client mutex is technically unnecessary here but we do it specifically to
        // demonstrate that holding it won't lead to self-deadlock.
        stdx::lock_guard<Client> lk(*cancelableOpCtx->getClient());
        cancelableOpCtx->markKilled(expectedErrorCode);
    }
    ASSERT_EQ(cancelableOpCtx->checkForInterruptNoAssert(), expectedErrorCode);
}

TEST_F(CancelableOperationContextTest, SafeWhenOperationContextKilledManually) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    auto expectedErrorCode = ErrorCodes::Error(5510298);
    {
        // Acquiring the Client mutex is technically unnecessary here but we do it specifically to
        // demonstrate that holding it won't lead to self-deadlock.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->markKilled(expectedErrorCode);
    }
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), expectedErrorCode);
}

}  // namespace
}  // namespace mongo
