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

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

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

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

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

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

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

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto result = future.get();
    ASSERT_EQ(externalState->callCount(), 2);
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result.at(shard0), 10);
}

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest, UnrecoverableErrorPropagatesWithoutRetry) {
    CancellationSource abortSource;
    auto externalState = std::make_shared<UnrecoverableMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, externalState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    auto status = future.getNoThrow();
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::InternalError);
    // A non-retryable error must not trigger any retry: exactly one call expected.
    ASSERT_EQ(externalState->callCount(), 1);
}

TEST_F(ReshardingRecipientPostCloningDeltaCollectorTest,
       AbortTokenCancelsBlockedFetchAndCompletesWithError) {
    CancellationSource abortSource;
    auto blockingState = std::make_shared<BlockingMockExternalState>();
    auto doc = makeDoc(CoordinatorStateEnum::kBlockingWrites, {ShardId("shard0")});
    auto collector = makeCollector(doc, blockingState, abortSource.token());

    auto future = collector->launch(scopedExecutor(), makeSpanForCollector());

    // Wait until getDocumentsDeltaFromRecipients is actually blocking before cancelling.
    blockingState->waitUntilEntered();

    abortSource.cancel();

    auto status = future.getNoThrow();
    ASSERT_NOT_OK(status);
}

}  // namespace
}  // namespace mongo
