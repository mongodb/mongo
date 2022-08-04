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

/**
** TESTS
*/

TEST_F(RangeDeleterServiceTest, InsertNewRangeDeletionTaskDocumentWithPendingFieldAsTrue) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
}

TEST_F(RangeDeleterServiceTest, InsertNewRangeDeletionTaskDocumentWithPendingFieldAsFalse) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed, false);

    insertRangeDeletionTaskDocument(opCtx, rdt);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);
}

TEST_F(RangeDeleterServiceTest, UpdateRangeDeletionTaskPendingFieldToFalse) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);

    updatePendingField(opCtx, rdt.getId(), false);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);
}

TEST_F(RangeDeleterServiceTest, UpdateRangeDeletionTaskPendingFieldToTrue) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);

    updatePendingField(opCtx, rdt.getId(), true);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
}

// SERVER-68552 TODO: Enable test after resolving the ticket
// TEST_F(RangeDeleterServiceTest, UnsetPendingFieldFromRangeDeletionTask) {
//     auto rds = RangeDeleterService::get(opCtx);
//     RangeDeletionTask rdt = createRangeDeletionTask(
//         uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

//     insertRangeDeletionTaskDocument(opCtx, rdt);
//     ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);

//     removePendingField(opCtx, rdt.getId());
//     ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);
// }

TEST_F(RangeDeleterServiceTest, RemoveRangeDeletionTask) {
    auto rds = RangeDeleterService::get(opCtx);
    RangeDeletionTask rdt = createRangeDeletionTask(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    insertRangeDeletionTaskDocument(opCtx, rdt);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);

    updatePendingField(opCtx, rdt.getId(), false);
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 1);

    deleteRangeDeletionTaskDocument(opCtx, rdt.getId());
    ASSERT_EQ(rds->getNumRangeDeletionTasksForCollection(uuidCollA), 0);
}

}  // namespace mongo
