// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/oplog_truncation.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

// The storage engine's pinned oplog timestamp is calculated as the minimum of the stable
// timestamp, the oldest active transaction timestamp, and pinned oplog timestamp.
//
// This function sets up the necessary state so that the storage engine will return 'ts' as the
// pinned oplog timestamp.
void setPinnedOplogTimestamp(OperationContext* opCtx, Timestamp ts) {
    auto svcCtx = opCtx->getServiceContext();
    svcCtx->getStorageEngine()->setOldestActiveTransactionTimestampCallback({});
    repl::StorageInterface::get(svcCtx)->setStableTimestamp(svcCtx, ts, true);
    repl::StorageInterface::get(svcCtx)->setPinnedOplogTimestamp(opCtx, ts);
}

class ComputeOplogTruncationBoundTest : public CatalogTestFixture {};

class ComputeOplogTruncationBoundWithReplicatedFastCountTest : public CatalogTestFixture {
public:
    ComputeOplogTruncationBoundWithReplicatedFastCountTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count::test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(replicated_fast_count::createReplicatedFastCountTimestampCollection(
            storageInterface(), operationContext()));
    }

    void setPersistedTimestamp(Timestamp ts) {
        auto [_, store] =
            replicated_fast_count::ReplicatedFastCountManager::get(getServiceContext())
                .getSizeCountStores_ForTest();
        replicated_fast_count::test_helpers::insertSizeCountTimestamp(
            operationContext(), *store, ts);
    }
};

TEST_F(ComputeOplogTruncationBoundTest, ReturnsPinnedOplogTimestamp) {
    const Timestamp pinnedOplogTs{1, 5};
    setPinnedOplogTimestamp(operationContext(), pinnedOplogTs);
    ASSERT_EQ(oplog_truncation::computeTruncationBound(operationContext()), pinnedOplogTs);
}

TEST_F(ComputeOplogTruncationBoundWithReplicatedFastCountTest,
       ReturnsValidAsOfIfLessThanPinnedOplog) {
    const Timestamp pinnedOplogTs{2, 0};
    const Timestamp persistedValidAsOfTs{1, 0};
    setPinnedOplogTimestamp(operationContext(), pinnedOplogTs);
    setPersistedTimestamp(persistedValidAsOfTs);
    ASSERT_EQ(oplog_truncation::computeTruncationBound(operationContext()), persistedValidAsOfTs);
}

TEST_F(ComputeOplogTruncationBoundWithReplicatedFastCountTest,
       ReturnsPinnedOplogIfLessThanValidAsOf) {
    const Timestamp pinnedOplogTs{1, 0};
    const Timestamp persistedValidAsOfTs{2, 0};
    setPinnedOplogTimestamp(operationContext(), pinnedOplogTs);
    setPersistedTimestamp(persistedValidAsOfTs);
    ASSERT_EQ(oplog_truncation::computeTruncationBound(operationContext()), pinnedOplogTs);
}

TEST_F(ComputeOplogTruncationBoundWithReplicatedFastCountTest,
       RetriesOnWriteConflictExceptionFromTimestampStore) {
    const Timestamp pinnedOplogTs{2, 0};
    const Timestamp persistedValidAsOfTs{1, 0};
    setPinnedOplogTimestamp(operationContext(), pinnedOplogTs);
    setPersistedTimestamp(persistedValidAsOfTs);

    // Throw WCE on the first call, then disable.
    FailPointEnableBlock fp("WTWriteConflictExceptionForReads",
                            FailPoint::ModeOptions{.mode = FailPoint::nTimes, .val = 1});

    // Should retry and return the correct value on the second attempt.
    ASSERT_EQ(oplog_truncation::computeTruncationBound(operationContext()), persistedValidAsOfTs);
}

}  // namespace
}  // namespace mongo
