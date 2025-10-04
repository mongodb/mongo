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

#include "mongo/base/string_data.h"
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
