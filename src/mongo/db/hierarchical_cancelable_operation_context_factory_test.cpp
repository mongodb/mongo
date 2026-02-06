/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"

#include "mongo/base/string_data.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class HierarchicalCancelableOperationContextTest : public unittest::Test {
public:
    void setUp() override {
        ThreadPool::Options options;
        options.poolName = "HierarchicalCancelableOperationContextTest";
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

TEST_F(HierarchicalCancelableOperationContextTest, KilledWhenCancellationSourceIsCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client =
        serviceCtx->getService()->makeClient("HierarchicalCancelableOperationContextTest");

    CancellationSource cancelSource;
    HierarchicalCancelableOperationContextFactory factory(cancelSource.token(), executor());

    ASSERT_EQ(factory.getHierarchyDepth(), 0);

    auto opCtx = factory.makeOperationContext(client.get());
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    cancelSource.cancel();
    waitForAllEarlierTasksToComplete();
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest,
       ChildTokensAreKilledWhenCancellationSourceIsCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client =
        serviceCtx->getService()->makeClient("HierarchicalCancelableOperationContextTest");

    CancellationSource cancelSource;
    HierarchicalCancelableOperationContextFactory factory(cancelSource.token(), executor());
    ASSERT_EQ(factory.getHierarchyDepth(), 0);

    auto childFactory = factory.createChild();

    ASSERT_EQ(childFactory->getHierarchyDepth(), 1);

    auto opCtx = childFactory->makeOperationContext(client.get());
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    cancelSource.cancel();
    waitForAllEarlierTasksToComplete();
    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest,
       KilledUponConstructionWhenCancellationSourceAlreadyCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client =
        serviceCtx->getService()->makeClient("HierarchicalCancelableOperationContextTest");

    shutDownExecutor();
    CancellationSource cancelSource;
    cancelSource.cancel();

    HierarchicalCancelableOperationContextFactory factory(cancelSource.token(), executor());
    auto opCtx = factory.makeOperationContext(client.get());

    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest,
       ChildTokensAreKilledUponConstructionWhenCancellationSourceAlreadyCanceled) {
    auto serviceCtx = ServiceContext::make();
    auto client =
        serviceCtx->getService()->makeClient("HierarchicalCancelableOperationContextTest");

    shutDownExecutor();
    CancellationSource cancelSource;
    cancelSource.cancel();

    HierarchicalCancelableOperationContextFactory factory(cancelSource.token(), executor());
    ASSERT_EQ(factory.getHierarchyDepth(), 0);

    auto childFactory = factory.createChild();
    ASSERT_EQ(childFactory->getHierarchyDepth(), 1);

    auto opCtx = childFactory->makeOperationContext(client.get());

    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest, HierarchyDepthTracking) {
    CancellationSource cancelSource;

    HierarchicalCancelableOperationContextFactory root(cancelSource.token(), executor());
    ASSERT_EQ(root.getHierarchyDepth(), 0);

    auto child = root.createChild();
    ASSERT_EQ(child->getHierarchyDepth(), 1);

    auto grandchild = child->createChild();
    ASSERT_EQ(grandchild->getHierarchyDepth(), 2);

    auto greatGrandchild = grandchild->createChild();
    ASSERT_EQ(greatGrandchild->getHierarchyDepth(), 3);

    ASSERT_EQ(root.getHierarchyDepth(), 0);
}

TEST_F(HierarchicalCancelableOperationContextTest, DeepHierarchyCancellationPropagation) {
    auto serviceCtx = ServiceContext::make();
    auto client =
        serviceCtx->getService()->makeClient("HierarchicalCancelableOperationContextTest");

    CancellationSource cancelSource;
    HierarchicalCancelableOperationContextFactory root(cancelSource.token(), executor());

    auto level1 = root.createChild();
    ASSERT_EQ(level1->getHierarchyDepth(), 1);

    auto level2 = level1->createChild();
    ASSERT_EQ(level2->getHierarchyDepth(), 2);

    auto level3 = level2->createChild();
    ASSERT_EQ(level3->getHierarchyDepth(), 3);

    auto level4 = level3->createChild();
    ASSERT_EQ(level4->getHierarchyDepth(), 4);

    auto opCtx = level4->makeOperationContext(client.get());
    ASSERT_OK(opCtx->checkForInterruptNoAssert());

    cancelSource.cancel();
    waitForAllEarlierTasksToComplete();

    ASSERT_EQ(opCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest, ThreeLevelHierarchyCancellation) {
    auto serviceCtx = ServiceContext::make();
    auto client1 = serviceCtx->getService()->makeClient("client1");
    auto client2 = serviceCtx->getService()->makeClient("client2");
    auto client3 = serviceCtx->getService()->makeClient("client3");

    CancellationSource cancelSource;
    HierarchicalCancelableOperationContextFactory root(cancelSource.token(), executor());

    auto child = root.createChild();
    ASSERT_EQ(child->getHierarchyDepth(), 1);

    auto grandchild = child->createChild();
    ASSERT_EQ(grandchild->getHierarchyDepth(), 2);

    auto rootOpCtx = root.makeOperationContext(client1.get());
    auto childOpCtx = child->makeOperationContext(client2.get());
    auto grandchildOpCtx = grandchild->makeOperationContext(client3.get());

    ASSERT_OK(rootOpCtx->checkForInterruptNoAssert());
    ASSERT_OK(childOpCtx->checkForInterruptNoAssert());
    ASSERT_OK(grandchildOpCtx->checkForInterruptNoAssert());

    cancelSource.cancel();
    waitForAllEarlierTasksToComplete();

    ASSERT_EQ(rootOpCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
    ASSERT_EQ(childOpCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
    ASSERT_EQ(grandchildOpCtx->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest, MultipleOperationContextsFromSameFactory) {
    auto serviceCtx = ServiceContext::make();
    auto client1 = serviceCtx->getService()->makeClient("client1");
    auto client2 = serviceCtx->getService()->makeClient("client2");
    auto client3 = serviceCtx->getService()->makeClient("client3");

    CancellationSource cancelSource;
    HierarchicalCancelableOperationContextFactory factory(cancelSource.token(), executor());

    auto opCtx1 = factory.makeOperationContext(client1.get());
    auto opCtx2 = factory.makeOperationContext(client2.get());
    auto opCtx3 = factory.makeOperationContext(client3.get());

    ASSERT_OK(opCtx1->checkForInterruptNoAssert());
    ASSERT_OK(opCtx2->checkForInterruptNoAssert());
    ASSERT_OK(opCtx3->checkForInterruptNoAssert());

    cancelSource.cancel();
    waitForAllEarlierTasksToComplete();

    ASSERT_EQ(opCtx1->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
    ASSERT_EQ(opCtx2->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
    ASSERT_EQ(opCtx3->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

TEST_F(HierarchicalCancelableOperationContextTest, ChildFactoriesFromDifferentParents) {
    auto serviceCtx = ServiceContext::make();
    auto client1 = serviceCtx->getService()->makeClient("client1");
    auto client2 = serviceCtx->getService()->makeClient("client2");

    CancellationSource cancelSource1;
    CancellationSource cancelSource2;

    HierarchicalCancelableOperationContextFactory factory1(cancelSource1.token(), executor());
    HierarchicalCancelableOperationContextFactory factory2(cancelSource2.token(), executor());

    auto child1 = factory1.createChild();
    auto child2 = factory2.createChild();

    auto opCtx1 = child1->makeOperationContext(client1.get());
    auto opCtx2 = child2->makeOperationContext(client2.get());

    ASSERT_OK(opCtx1->checkForInterruptNoAssert());
    ASSERT_OK(opCtx2->checkForInterruptNoAssert());

    cancelSource1.cancel();
    waitForAllEarlierTasksToComplete();

    ASSERT_EQ(opCtx1->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
    ASSERT_OK(opCtx2->checkForInterruptNoAssert());

    cancelSource2.cancel();
    waitForAllEarlierTasksToComplete();

    ASSERT_EQ(opCtx2->checkForInterruptNoAssert(), ErrorCodes::Interrupted);
}

}  // namespace
}  // namespace mongo
