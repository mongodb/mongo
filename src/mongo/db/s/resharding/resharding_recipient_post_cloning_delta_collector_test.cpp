// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_recipient_post_cloning_delta_collector.h"

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

class ReshardingRecipientPostCloningDeltaCollectorTest
    : public resharding_delta_collector_test_util::PostCloningDeltaCollectorTestBase {
public:
    /**
     * Builds a ReshardingCoordinatorDocument in the given state.
     * If documentsFinalAlreadySet is true, sets documentsFinal on each recipient, simulating a
     * resume after the fetch already completed.
     */
    ReshardingCoordinatorDocument makeDoc(CoordinatorStateEnum state,
                                          const std::vector<ShardId>& recipientShardIds,
                                          bool documentsFinalAlreadySet = false) {
        std::vector<RecipientShardEntry> recipientShards;
        for (const auto& shardId : recipientShardIds) {
            RecipientShardEntry entry(shardId, {});
            if (documentsFinalAlreadySet) {
                entry.setDocumentsFinal(100);
            }
            recipientShards.emplace_back(std::move(entry));
        }

        ReshardingCoordinatorDocument doc;
        CommonReshardingMetadata commonMetadata{
            UUID::gen(),
            NamespaceString::createNamespaceString_forTest("db.coll"),
            UUID::gen(),
            NamespaceString::createNamespaceString_forTest("db.tmp"),
            BSON("x" << 1)};

        ForwardableOperationMetadata fom;
        fom.setVersionContext(
            VersionContext{serverGlobalParams.featureCompatibility.acquireFCVSnapshot()});
        commonMetadata.setForwardableOpMetadata(std::move(fom));

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        doc.setState(state);
        doc.setRecipientShards(std::move(recipientShards));
        return doc;
    }

    std::shared_ptr<ReshardingRecipientPostCloningDeltaCollector> makeCollector(
        ReshardingCoordinatorDocument doc,
        std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
        CancellationToken abortToken) {
        return std::make_shared<ReshardingRecipientPostCloningDeltaCollector>(
            std::move(doc),
            std::move(externalState),
            abortToken,
            std::make_unique<HierarchicalCancelableOperationContextFactory>(
                abortToken, executorForCancellation()));
    }
};

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest, SkipsFetchWhenNotInBlockingWritesState) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<SuccessMockExternalState>(
        std::map<ShardId, int64_t>{{ShardId("shard0"), 5}});
    auto doc = makeDoc(CoordinatorStateEnum::kCommitting, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = launchCollector(collector);

    auto result = future.get();
    ASSERT_TRUE(result.empty());
}

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest, SkipsFetchWhenDocumentsFinalAlreadySet) {
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

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest, FetchesDeltaSuccessfully) {
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

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest,
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

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest, UnrecoverableErrorPropagatesWithoutRetry) {
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

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest,
       AbortTokenCancelsBlockedFetchAndCompletesWithError) {
    CancellationSource abortSource;
    auto blockingState = std::make_shared<BlockingMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, blockingState, abortSource.token());

    auto future = launchCollector(collector);

    // Wait until getDocumentsDeltaFromRecipients is actually blocking before cancelling.
    blockingState->waitUntilEntered();

    abortSource.cancel();

    auto status = future.getNoThrow();
    ASSERT_NOT_OK(status);
}

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest,
       LaunchFutureResolvesWhenExecutorIsShutDown) {
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
