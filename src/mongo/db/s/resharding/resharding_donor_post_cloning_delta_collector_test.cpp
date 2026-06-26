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

#include "mongo/db/s/resharding/resharding_donor_post_cloning_delta_collector.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/s/resharding/resharding_delta_collector_test_util.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using resharding_delta_collector_test_util::BlockingMockExternalState;
using resharding_delta_collector_test_util::FailOnceThenSucceedMockExternalState;
using resharding_delta_collector_test_util::SuccessMockExternalState;
using resharding_delta_collector_test_util::UnrecoverableMockExternalState;


class ReshardingDonorPostCloningDeltaCollectorTest
    : public resharding_delta_collector_test_util::PostCloningDeltaCollectorTestBase {
public:
    /**
     * Builds a ReshardingCoordinatorDocument in the given state.
     * If documentsFinalAlreadySet is true, sets documentsFinal on the first donor, simulating a
     * resume after the fetch already completed.
     */
    ReshardingCoordinatorDocument makeDoc(CoordinatorStateEnum state,
                                          const std::vector<ShardId>& donorShardIds,
                                          bool documentsFinalAlreadySet = false) {
        std::vector<DonorShardEntry> donorShards;
        for (const auto& shardId : donorShardIds) {
            DonorShardEntry entry(shardId, {});
            if (documentsFinalAlreadySet) {
                entry.setDocumentsFinal(100);
            }
            donorShards.emplace_back(std::move(entry));
        }

        ReshardingCoordinatorDocument doc;
        doc.setCommonReshardingMetadata({UUID::gen(),
                                         NamespaceString::createNamespaceString_forTest("db.coll"),
                                         UUID::gen(),
                                         NamespaceString::createNamespaceString_forTest("db.tmp"),
                                         BSON("x" << 1)});
        doc.setState(state);
        doc.setDonorShards(std::move(donorShards));
        return doc;
    }

    std::shared_ptr<ReshardingDonorPostCloningDeltaCollector> makeCollector(
        ReshardingCoordinatorDocument doc,
        std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
        CancellationToken abortToken) {
        return std::make_shared<ReshardingDonorPostCloningDeltaCollector>(
            std::move(doc),
            std::move(externalState),
            abortToken,
            std::make_unique<HierarchicalCancelableOperationContextFactory>(
                abortToken, executorForCancellation()));
    }
};

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, SkipsFetchWhenNotInBlockingWritesState) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{ShardId("shard0"), 5}});
    auto doc = makeDoc(CoordinatorStateEnum::kCommitting, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = launchCollector(collector);

    auto result = future.get();
    ASSERT_TRUE(result.empty());
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, SkipsFetchWhenDocumentsFinalAlreadySet) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{ShardId("shard0"), 5}});
    // Simulate a resume after the delta was already persisted.
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites,
                       {ShardId("shard0")},
                       true /* documentsFinalAlreadySet */);
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = launchCollector(collector);

    auto result = future.get();
    ASSERT_TRUE(result.empty());
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, FetchesDeltaSuccessfully) {
    const ShardId shard0("shard0");
    const ShardId shard1("shard1");
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{shard0, 42}, {shard1, -7}});

    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {shard0, shard1});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = launchCollector(collector);

    auto result = future.get();
    ASSERT_EQ(result.size(), 2U);
    ASSERT_EQ(result.at(shard0), 42);
    ASSERT_EQ(result.at(shard1), -7);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest,
       RetryableNetworkErrorTriggersRetryAndSucceeds) {
    const ShardId shard0("shard0");
    CancellationSource abortSource;
    auto externalState = std::make_shared<FailOnceThenSucceedMockExternalState>(
        std::map<ShardId, int64_t>{{shard0, 10}});

    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {shard0});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    int retryCount = 0;
    auto future = collector->launch(
        scopedExecutor(), makeSpanForCollector(), [&retryCount] { ++retryCount; });

    auto result = future.get();
    ASSERT_EQ(externalState->callCount(), 2);
    ASSERT_EQ(retryCount, 1);
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result.at(shard0), 10);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, UnrecoverableErrorPropagatesWithoutRetry) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<UnrecoverableMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    int retryCount = 0;
    auto future = collector->launch(
        scopedExecutor(), makeSpanForCollector(), [&retryCount] { ++retryCount; });

    auto status = future.getNoThrow();
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::InternalError);
    // A non-retryable error must not trigger any retry: exactly one call expected.
    ASSERT_EQ(externalState->callCount(), 1);
    ASSERT_EQ(retryCount, 0);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest,
       AbortTokenCancelsBlockedFetchAndCompletesWithError) {
    CancellationSource abortSource;
    auto blockingState = std::make_shared<BlockingMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, blockingState, abortSource.token());

    auto future = launchCollector(collector);

    // Wait until getDocumentsDeltaFromDonors is actually blocking before cancelling.
    blockingState->waitUntilEntered();

    abortSource.cancel();

    auto status = future.getNoThrow();
    ASSERT_NOT_OK(status);
}

TEST_F(ReshardingDonorPostCloningDeltaCollectorTest, LaunchFutureResolvesWhenExecutorIsShutDown) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{ShardId("shard0"), 5}});
    // kCommitting skips the fetch, leaving only launch's getAsync completion callback to exercise.
    auto doc = makeDoc(CoordinatorStateEnum::kCommitting, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    // With a shut-down executor, a plain getAsync drops its callback and the future hangs;
    // unsafeToInlineFuture() runs it inline so launch() still resolves.
    (*scopedExecutor())->shutdown();

    auto status = launchCollector(collector).getNoThrow();
    ASSERT_OK(status);
    ASSERT_TRUE(status.getValue().empty());
}

}  // namespace
}  // namespace mongo
