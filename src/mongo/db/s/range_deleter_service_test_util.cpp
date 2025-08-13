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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
                                          boost::optional<KeyPattern> keyPattern,
                                          const ChunkVersion& shardVersion) {
    RangeDeletionTask rdt;
    rdt.setId(UUID::gen());
    rdt.setNss(RangeDeleterServiceTest::nssWithUuid[collectionUUID]);
    rdt.setDonorShardId(ShardId("shard0"));
    rdt.setCollectionUuid(collectionUUID);
    rdt.setRange(ChunkRange(min, max));
    rdt.setWhenToClean(whenToClean);
    rdt.setPending(pending);
    rdt.setKeyPattern(keyPattern);
    rdt.setPreMigrationShardVersion(shardVersion);

    return rdt;
}

std::shared_ptr<RangeDeletionWithOngoingQueries> createRangeDeletionTaskWithOngoingQueries(
    const UUID& collectionUUID,
    const BSONObj& min,
    const BSONObj& max,
    CleanWhenEnum whenToClean,
    bool pending,
    boost::optional<KeyPattern> keyPattern,
    const ChunkVersion& collectionVersion) {
    return std::make_shared<RangeDeletionWithOngoingQueries>(createRangeDeletionTask(
        collectionUUID, min, max, whenToClean, pending, keyPattern, collectionVersion));
}

SharedSemiFuture<void> registerAndCreatePersistentTask(
    OperationContext* opCtx,
    const RangeDeletionTask& rdt,
    SemiFuture<void>&& waitForActiveQueriesToComplete) {
    auto rds = RangeDeleterService::get(opCtx);

    // Register task as `pending` in order to block it until the persistent document is non-pending
    auto completionFuture = rds->registerTask(
        rdt, std::move(waitForActiveQueriesToComplete), false /* fromStepUp */, true /* pending*/);

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
        dbclient.insert(nss, BSON(RangeDeleterServiceTest::kShardKey << nextI));
    }
    return maxCount;
}

void insertRangeDeletionTaskDocument(OperationContext* opCtx, const RangeDeletionTask& rdt) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    // Randomly persist document via insert or upsert. Testing both scenarios because migrations
    // insert/update range deletion documents while rename participants could potentially insert
    // via upsert. This is a safeguard test to make sure that the insert observer is properly
    // invoked in both cases: if at some point inserts via upserts would not be observed anymore
    // as normal inserts, the range deleter would break.
    PseudoRandom random(SecureRandom().nextInt64());
    if (random.nextInt32() % 2) {
        store.add(opCtx, rdt);
    } else {
        store.upsert(opCtx,
                     BSON(RangeDeletionTask::kIdFieldName << rdt.getId()),
                     BSON("$set" << rdt.toBSON()));
    }
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
        auto chunkRange = ChunkRange::fromBSON(chunkRanges[i].Obj());
        ASSERT(chunkRange == expectedChunkRanges[i])
            << "Expected " << chunkRange.toBSON() << " == " << expectedChunkRanges[i].toBSON();
    }
}

void verifyProcessingFlag(OperationContext* opCtx,
                          UUID uuidColl,
                          const ChunkRange& range,
                          bool processingExpected) {
    DBDirectClient client(opCtx);

    const auto query = BSON(
        RangeDeletionTask::kCollectionUuidFieldName
        << uuidColl << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinFieldName
        << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxFieldName
        << range.getMax());

    FindCommandRequest findRequest{NamespaceString::kRangeDeletionNamespace};
    findRequest.setFilter(query);
    auto doc = client.findOne(std::move(findRequest));

    ASSERT(!doc.isEmpty()) << "Chunk '" << query << "' not found";

    if (processingExpected) {
        ASSERT_EQ(true, doc.getField(RangeDeletionTask::kProcessingFieldName).booleanSafe())
            << "The `processing` field was expected to be true for that chunk. Chunk doc found: "
            << doc;
    } else {
        ASSERT(!doc.hasField(RangeDeletionTask::kProcessingFieldName))
            << "The `processing` field was not expected to be present for that chunk. Chunk doc "
               "found: "
            << doc;
    }
}

void _clearFilteringMetadataByUUID(OperationContext* opCtx, const UUID& uuid) {
    NamespaceString nss = RangeDeleterServiceTest::nssWithUuid[uuid];

    AutoGetCollection autoColl(opCtx, nss, LockMode::MODE_X);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->clearFilteringMetadata(opCtx);
}

}  // namespace mongo
