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

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/range_deleter_service_test.h"

namespace mongo {

void insertRangeDeletionTaskDocument(OperationContext* opCtx, const RangeDeletionTask& rdt) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.add(opCtx, rdt);
}

void updatePendingField(OperationContext* opCtx, UUID migrationId, bool pending) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.update(opCtx,
                 BSON(RangeDeletionTask::kIdFieldName << migrationId),
                 BSON("$set" << BSON(RangeDeletionTask::kPendingFieldName << pending)));
}

void removePendingField(OperationContext* opCtx, UUID migrationId) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.update(opCtx,
                 BSON(RangeDeletionTask::kIdFieldName << migrationId),
                 BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << "")));
}

void deleteRangeDeletionTaskDocument(OperationContext* opCtx, UUID migrationId) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.remove(opCtx, BSON(RangeDeletionTask::kIdFieldName << migrationId));
}

// Ensure that `expectedChunkRanges` range deletion tasks are scheduled for collection with UUID
// `uuidColl`
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

/**
** TESTS
*/

TEST_F(RangeDeleterServiceTest, InsertNewRangeDeletionTaskDocumentWithPendingFieldAsTrue) {
    auto rds = RangeDeleterService::get(opCtx);

    RangeDeletionTask rdt1 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);
    RangeDeletionTask rdt2 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 10), BSON("a" << 20), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt1);
    insertRangeDeletionTaskDocument(opCtx, rdt2);

    verifyRangeDeletionTasks(opCtx, uuidCollA, {});

    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
}

TEST_F(RangeDeleterServiceTest, InsertNewRangeDeletionTaskDocumentWithPendingFieldAsFalse) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt1 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed, false);
    RangeDeletionTask rdt2 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 10), BSON("a" << 20), CleanWhenEnum::kDelayed, false);

    insertRangeDeletionTaskDocument(opCtx, rdt1);
    insertRangeDeletionTaskDocument(opCtx, rdt2);

    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange(), rdt2.getRange()});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {});

    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 2);
}

TEST_F(RangeDeleterServiceTest, UpdateRangeDeletionTaskPendingFieldToFalse) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt1 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);
    RangeDeletionTask rdt2 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 10), BSON("a" << 20), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt1);
    insertRangeDeletionTaskDocument(opCtx, rdt2);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollB), 0);

    updatePendingField(opCtx, rdt2.getId(), false);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt2.getRange()});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {});

    updatePendingField(opCtx, rdt1.getId(), false);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange(), rdt2.getRange()});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {});

    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 2);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollB), 0);
}

TEST_F(RangeDeleterServiceTest, UpdateRangeDeletionTaskPendingFieldToTrue) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);

    updatePendingField(opCtx, rdt.getId(), true);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
}

TEST_F(RangeDeleterServiceTest, UnsetPendingFieldFromRangeDeletionTask) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt1 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);
    RangeDeletionTask rdt2 = createRangeDeletionTask(
        uuidCollB, BSON("a" << 10), BSON("a" << 20), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt1);
    insertRangeDeletionTaskDocument(opCtx, rdt2);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollB), 0);

    removePendingField(opCtx, rdt1.getId());
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange()});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollB), 0);

    removePendingField(opCtx, rdt2.getId());
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange()});
    verifyRangeDeletionTasks(opCtx, uuidCollB, {rdt2.getRange()});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollB), 1);
}

TEST_F(RangeDeleterServiceTest, RemoveRangeDeletionTask) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt1 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 5), BSON("a" << 15), CleanWhenEnum::kDelayed);
    RangeDeletionTask rdt2 = createRangeDeletionTask(
        uuidCollA, BSON("a" << 15), BSON("a" << 20), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt1);
    insertRangeDeletionTaskDocument(opCtx, rdt2);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);

    updatePendingField(opCtx, rdt1.getId(), false);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange()});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);

    updatePendingField(opCtx, rdt2.getId(), false);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange(), rdt2.getRange()});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 2);

    deleteRangeDeletionTaskDocument(opCtx, rdt2.getId());
    verifyRangeDeletionTasks(opCtx, uuidCollA, {rdt1.getRange()});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);

    deleteRangeDeletionTaskDocument(opCtx, rdt1.getId());
    verifyRangeDeletionTasks(opCtx, uuidCollA, {});
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
}

}  // namespace mongo
