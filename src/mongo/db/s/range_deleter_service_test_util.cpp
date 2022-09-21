/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/range_deleter_service_test.h"

namespace mongo {

/**
 *  RangeDeletionWithOngoingQueries implementation
 */
RangeDeletionWithOngoingQueries::RangeDeletionWithOngoingQueries(const RangeDeletionTask& t)
    : _task(t) {}

RangeDeletionTask RangeDeletionWithOngoingQueries::getTask() {
    return _task;
}

void RangeDeletionWithOngoingQueries::drainOngoingQueries() {
    _ongoingQueries.setFrom(Status::OK());
}

SemiFuture<void> RangeDeletionWithOngoingQueries::getOngoingQueriesFuture() {
    return _ongoingQueries.getFuture().semi();
}

/**
 * Utils
 */
RangeDeletionTask createRangeDeletionTask(const UUID& collectionUUID,
                                          const BSONObj& min,
                                          const BSONObj& max,
                                          CleanWhenEnum whenToClean,
                                          bool pending,
                                          boost::optional<KeyPattern> keyPattern) {
    RangeDeletionTask rdt;
    rdt.setId(UUID::gen());
    rdt.setNss(RangeDeleterServiceTest::nssWithUuid[collectionUUID]);
    rdt.setDonorShardId(ShardId("shard0"));
    rdt.setCollectionUuid(collectionUUID);
    rdt.setRange(ChunkRange(min, max));
    rdt.setWhenToClean(whenToClean);
    rdt.setPending(pending);
    rdt.setKeyPattern(keyPattern);

    return rdt;
}

std::shared_ptr<RangeDeletionWithOngoingQueries> createRangeDeletionTaskWithOngoingQueries(
    const UUID& collectionUUID,
    const BSONObj& min,
    const BSONObj& max,
    CleanWhenEnum whenToClean,
    bool pending,
    boost::optional<KeyPattern> keyPattern) {
    return std::make_shared<RangeDeletionWithOngoingQueries>(
        createRangeDeletionTask(collectionUUID, min, max, whenToClean, pending, keyPattern));
}

// TODO review this method: the task may be registered, finish and be recreated by inserting the
// document
SharedSemiFuture<void> registerAndCreatePersistentTask(
    OperationContext* opCtx,
    const RangeDeletionTask& rdt,
    SemiFuture<void>&& waitForActiveQueriesToComplete) {
    auto rds = RangeDeleterService::get(opCtx);

    auto completionFuture = rds->registerTask(rdt, std::move(waitForActiveQueriesToComplete));

    // Range deletion task will only proceed if persistent doc exists and its `pending` field
    // doesn't exist
    insertRangeDeletionTaskDocument(opCtx, rdt);
    removePendingField(opCtx, rdt.getId());

    return completionFuture;
}

int insertDocsWithinRange(
    OperationContext* opCtx, const NamespaceString& nss, int min, int max, int maxCount) {

    DBDirectClient dbclient(opCtx);
    for (auto i = 0; i < maxCount; ++i) {
        const int nextI = min + i;
        if (nextI == max) {
            return i;
        }
        dbclient.insert(nss.toString(), BSON(RangeDeleterServiceTest::kShardKey << nextI));
    }
    return maxCount;
}


void insertRangeDeletionTaskDocument(OperationContext* opCtx, const RangeDeletionTask& rdt) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.add(opCtx, rdt);
}

void updatePendingField(OperationContext* opCtx, UUID rdtId, bool pending) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.update(opCtx,
                 BSON(RangeDeletionTask::kIdFieldName << rdtId),
                 BSON("$set" << BSON(RangeDeletionTask::kPendingFieldName << pending)));
}

void removePendingField(OperationContext* opCtx, UUID rdtId) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.update(opCtx,
                 BSON(RangeDeletionTask::kIdFieldName << rdtId),
                 BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << "")));
}

void deleteRangeDeletionTaskDocument(OperationContext* opCtx, UUID rdtId) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.remove(opCtx, BSON(RangeDeletionTask::kIdFieldName << rdtId));
}

/**
 * Ensure that `expectedChunkRanges` range deletion tasks are scheduled for collection with UUID
 * `uuidColl`
 */
void verifyRangeDeletionTasks(OperationContext* opCtx,
                              UUID uuidColl,
                              std::vector<ChunkRange> expectedChunkRanges) {
    auto rds = RangeDeleterService::get(opCtx);

    // Get chunk ranges inserted to be deleted by RangeDeleterService
    BSONObj dumpState = rds->dumpState();
    BSONElement chunkRangesElem = dumpState.getField(uuidColl.toString());
    if (!chunkRangesElem.ok() && expectedChunkRanges.size() == 0) {
        return;
    }
    ASSERT(chunkRangesElem.ok()) << "Expected to find range deletion tasks from collection "
                                 << uuidColl.toString();

    const auto chunkRanges = chunkRangesElem.Array();
    ASSERT_EQ(chunkRanges.size(), expectedChunkRanges.size());

    // Sort expectedChunkRanges vector to replicate RangeDeleterService dumpState order
    struct {
        bool operator()(const ChunkRange& a, const ChunkRange& b) {
            return a.getMin().woCompare(b.getMin()) < 0;
        }
    } RANGES_COMPARATOR;

    std::sort(expectedChunkRanges.begin(), expectedChunkRanges.end(), RANGES_COMPARATOR);

    // Check expectedChunkRanges are exactly the same as the returned ones
    for (size_t i = 0; i < expectedChunkRanges.size(); ++i) {
        ASSERT(ChunkRange::fromBSONThrowing(chunkRanges[i].Obj()) == expectedChunkRanges[i])
            << "Expected " << ChunkRange::fromBSONThrowing(chunkRanges[i].Obj()).toBSON()
            << " == " << expectedChunkRanges[i].toBSON();
    }
}

void _clearFilteringMetadataByUUID(OperationContext* opCtx, const UUID& uuid) {
    NamespaceString nss = RangeDeleterServiceTest::nssWithUuid[uuid];

    AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_X);
    CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);
}

}  // namespace mongo
