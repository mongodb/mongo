// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/cancelable_operation_context.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");
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
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

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
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

    shutDownExecutor();
    CancellationSource cancelSource;
    cancelSource.cancel();

    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(CancelableOperationContextTest, SafeWhenCancellationSourceIsCanceledUnderClientMutex) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    {
        // Holding the Client mutex while canceling the CancellationSource won't lead to
        // self-deadlock.
        std::lock_guard<Client> lk(*opCtx->getClient());
        cancelSource.cancel();
    }
    waitForAllEarlierTasksToComplete();
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(CancelableOperationContextTest, SafeWhenDestructedBeforeCancellationSourceIsCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    boost::optional<CancelableOperationContext> opCtx;
    opCtx.emplace(client->makeOperationContext(), cancelSource.token(), executor());

    opCtx.reset();
    cancelSource.cancel();
}

TEST_F(CancelableOperationContextTest, NotKilledWhenCancellationSourceIsDestructed) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

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
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

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
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

    auto opCtx = client->makeOperationContext();
    auto cancelToken = opCtx->getCancellationToken();
    auto cancelableOpCtx = CancelableOperationContext{std::move(opCtx), cancelToken, executor()};

    ASSERT_OK(cancelableOpCtx->checkForInterruptNoAssert());

    auto expectedErrorCode = ErrorCodes::Error(5510299);
    {
        // Acquiring the Client mutex is technically unnecessary here but we do it specifically to
        // demonstrate that holding it won't lead to self-deadlock.
        std::lock_guard<Client> lk(*cancelableOpCtx->getClient());
        cancelableOpCtx->markKilled(expectedErrorCode);
    }
    ASSERT_EQ(cancelableOpCtx->checkForInterruptNoAssert(), expectedErrorCode);
}

TEST_F(CancelableOperationContextTest, SafeWhenOperationContextKilledManually) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("CancelableOperationContextTest");

    CancellationSource cancelSource;
    auto opCtx = CancelableOperationContext{
        client->makeOperationContext(), cancelSource.token(), executor()};

    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    auto expectedErrorCode = ErrorCodes::Error(5510298);
    {
        // Acquiring the Client mutex is technically unnecessary here but we do it specifically to
        // demonstrate that holding it won't lead to self-deadlock.
        std::lock_guard<Client> lk(*opCtx->getClient());
        opCtx->markKilled(expectedErrorCode);
    }
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), expectedErrorCode);
}

}  // namespace
}  // namespace mongo
