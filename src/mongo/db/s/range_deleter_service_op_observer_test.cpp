// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

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

}  // namespace mongo
