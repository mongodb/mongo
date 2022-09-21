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
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/log_test.h"

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
    SemiFuture<void> getOngoingQueriesFuture();

private:
    RangeDeletionTask _task;
    SharedPromise<void> _ongoingQueries;
};

class RangeDeleterServiceTest : public ShardServerTestFixture {
public:
    void setUp() override;
    void tearDown() override;

    OperationContext* opCtx;

    // Instantiate some collection UUIDs and tasks to be used for testing
    UUID uuidCollA = UUID::gen();
    inline static const NamespaceString nsCollA{"test", "collA"};
    UUID uuidCollB = UUID::gen();
    inline static const NamespaceString nsCollB{"test", "collB"};

    inline static std::map<UUID, NamespaceString> nssWithUuid{};

    std::shared_ptr<RangeDeletionWithOngoingQueries> rangeDeletionTask0ForCollA;
    std::shared_ptr<RangeDeletionWithOngoingQueries> rangeDeletionTask1ForCollA;
    std::shared_ptr<RangeDeletionWithOngoingQueries> rangeDeletionTask0ForCollB;

    inline static const std::string kShardKey = "_id";
    inline static const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

private:
    void _setFilteringMetadataByUUID(OperationContext* opCtx, const UUID& uuid);

    // Scoped objects
    RAIIServerParameterControllerForTest enableFeatureFlag{"featureFlagRangeDeleterService", true};
    unittest::MinimumLoggedSeverityGuard _severityGuard{logv2::LogComponent::kShardingRangeDeleter,
                                                        logv2::LogSeverity::Debug(2)};
};

RangeDeletionTask createRangeDeletionTask(const UUID& collectionUUID,
                                          const BSONObj& min,
                                          const BSONObj& max,
                                          CleanWhenEnum whenToClean = CleanWhenEnum::kNow,
                                          bool pending = true,
                                          boost::optional<KeyPattern> keyPattern = boost::none);

std::shared_ptr<RangeDeletionWithOngoingQueries> createRangeDeletionTaskWithOngoingQueries(
    const UUID& collectionUUID,
    const BSONObj& min,
    const BSONObj& max,
    CleanWhenEnum whenToClean = CleanWhenEnum::kNow,
    bool pending = true,
    boost::optional<KeyPattern> keyPattern = boost::none);

SharedSemiFuture<void> registerAndCreatePersistentTask(
    OperationContext* opCtx,
    const RangeDeletionTask& rdt,
    SemiFuture<void>&& waitForActiveQueriesToComplete);

int insertDocsWithinRange(
    OperationContext* opCtx, const NamespaceString& nss, int min, int max, int maxCount);

void verifyRangeDeletionTasks(OperationContext* opCtx,
                              UUID uuidColl,
                              std::vector<ChunkRange> expectedChunkRanges);

/* Unset any filtering metadata associated with the specified collection */
void _clearFilteringMetadataByUUID(OperationContext* opCtx, const UUID& uuid);

// CRUD operation over `config.rangeDeletions`
void insertRangeDeletionTaskDocument(OperationContext* opCtx, const RangeDeletionTask& rdt);
void updatePendingField(OperationContext* opCtx, UUID rdtId, bool pending);
void removePendingField(OperationContext* opCtx, UUID rdtId);
void deleteRangeDeletionTaskDocument(OperationContext* opCtx, UUID rdtId);

}  // namespace mongo
