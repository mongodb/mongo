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
