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
#pragma once

#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/shard_server_test_fixture.h"

namespace mongo {

/*
 * Utility class enclosing a range deletion task and the associated active queries completion
 * promise/future
 */
class RangeDeletionWithOngoingQueries {
public:
    RangeDeletionWithOngoingQueries(const RangeDeletionTask& t);

    RangeDeletionTask getTask();
    void drainOngoingQueries();
    auto getOngoingQueriesFuture();

private:
    RangeDeletionTask _task;
    SharedPromise<void> _ongoingQueries;
};

class RangeDeleterServiceTest : public ShardServerTestFixture {
public:
    void setUp() override;

    void tearDown() override;

    OperationContext* opCtx;

    // Util methods
    RangeDeletionTask createRangeDeletionTask(const UUID& collectionUUID,
                                              const BSONObj& min,
                                              const BSONObj& max,
                                              CleanWhenEnum whenToClean = CleanWhenEnum::kNow,
                                              bool pending = true);

    RangeDeletionWithOngoingQueries createRangeDeletionTaskWithOngoingQueries(
        const UUID& collectionUUID,
        const BSONObj& min,
        const BSONObj& max,
        CleanWhenEnum whenToClean = CleanWhenEnum::kNow,
        bool pending = true);

    // Instantiate some collection UUIDs and tasks to be used for testing
    const UUID uuidCollA = UUID::gen();
    RangeDeletionWithOngoingQueries rangeDeletionTask0ForCollA =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));
    RangeDeletionWithOngoingQueries rangeDeletionTask1ForCollA =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 10), BSON("a" << 20));
    const UUID uuidCollB = UUID::gen();
    RangeDeletionWithOngoingQueries rangeDeletionTask0ForCollB =
        createRangeDeletionTaskWithOngoingQueries(uuidCollB, BSON("a" << 0), BSON("a" << 10));
};

}  // namespace mongo
