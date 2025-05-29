/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_coordinator_dao.h"

#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace resharding {

class SpyingDocumentUpdater : public DaoStorageClient {
public:
    void alterState(OperationContext* opCtx, const BatchedCommandRequest& request) override {
        _lastRequest = request.toBSON();
    }

    const BSONObj& getLastRequest() {
        return _lastRequest;
    }

    ReshardingCoordinatorDocument readState(OperationContext* opCtx,
                                            const UUID& reshardingUUID) override {
        return _state;
    }

    ReshardingCoordinatorDocument& getOnDiskStateForModification() {
        return _state;
    }

private:
    BSONObj _lastRequest;
    ReshardingCoordinatorDocument _state;
};

TEST(ReshardingCoordinatorDaoTest, TransitionToCloningPhase) {
    ClockSourceMock clock;
    SpyingDocumentUpdater updater;
    updater.getOnDiskStateForModification().setState(CoordinatorStateEnum::kPreparingToDonate);
    ReshardingCoordinatorDao dao;
    OperationContext* opCtx = nullptr;

    auto uuid = UUID::gen();
    auto cloneTimestamp = Timestamp(10, 50);

    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(100);
    approxCopySize.setApproxDocumentsToCopy(5);

    auto cloneStartTime = clock.now();
    dao.transitionToCloningPhase(
        opCtx, &updater, cloneStartTime, cloneTimestamp, approxCopySize, uuid);
    const auto& lastRequest = updater.getLastRequest();

    auto expectedUpdates = BSON_ARRAY(BSON(
        "q" << BSON("_id" << uuid) << "u"
            << BSON("$set" << BSON("state" << "cloning"
                                           << "cloneTimestamp" << cloneTimestamp
                                           << "approxBytesToCopy" << 100 << "approxDocumentsToCopy"
                                           << 5 << "metrics.documentCopy.start" << cloneStartTime))
            << "multi" << false << "upsert" << false));

    ASSERT_EQUALS(lastRequest.getStringField("update"),
                  NamespaceString::kConfigReshardingOperationsNamespace.coll());
    auto updates = lastRequest.getObjectField("updates");
    ASSERT_BSONOBJ_EQ(updates, expectedUpdates);
}

DEATH_TEST(ReshardingCoordinatorDaoTest,
           TransitionToCloningPhasePreviousStateInvariant,
           "invariant") {
    ClockSourceMock clock;
    SpyingDocumentUpdater updater;
    updater.getOnDiskStateForModification().setState(CoordinatorStateEnum::kCloning);
    ReshardingCoordinatorDao dao;
    OperationContext* opCtx = nullptr;

    auto uuid = UUID::gen();
    auto cloneTimestamp = Timestamp(10, 50);

    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(100);
    approxCopySize.setApproxDocumentsToCopy(5);

    auto cloneStartTime = clock.now();
    dao.transitionToCloningPhase(
        opCtx, &updater, cloneStartTime, cloneTimestamp, approxCopySize, uuid);
}

TEST(ReshardingCoordinatorDaoTest, TransitionToApplyingPhase) {
    ClockSourceMock clock;
    SpyingDocumentUpdater updater;
    updater.getOnDiskStateForModification().setState(CoordinatorStateEnum::kCloning);
    ReshardingCoordinatorDao dao;
    OperationContext* opCtx = nullptr;

    auto uuid = UUID::gen();
    auto applyStartTime = clock.now();
    dao.transitionToApplyingPhase(opCtx, &updater, applyStartTime, uuid);
    const auto& lastRequest = updater.getLastRequest();

    auto expectedUpdates = BSON_ARRAY(BSON(
        "q" << BSON("_id" << uuid) << "u"
            << BSON("$set" << BSON("state" << "applying"
                                           << "metrics.documentCopy.stop" << applyStartTime
                                           << "metrics.oplogApplication.start" << applyStartTime))
            << "multi" << false << "upsert" << false));

    ASSERT_EQUALS(lastRequest.getStringField("update"),
                  NamespaceString::kConfigReshardingOperationsNamespace.coll());
    auto updates = lastRequest.getObjectField("updates");
    ASSERT_BSONOBJ_EQ(updates, expectedUpdates);
}

DEATH_TEST(ReshardingCoordinatorDaoTest,
           TransitionToApplyingPhasePreviousStateInvariant,
           "invariant") {
    ClockSourceMock clock;
    SpyingDocumentUpdater updater;
    updater.getOnDiskStateForModification().setState(CoordinatorStateEnum::kApplying);
    ReshardingCoordinatorDao dao;
    OperationContext* opCtx = nullptr;

    auto uuid = UUID::gen();
    auto applyStartTime = clock.now();
    dao.transitionToApplyingPhase(opCtx, &updater, applyStartTime, uuid);
}

TEST(ReshardingCoordinatorDaoTest, TransitionToBlockingWritesPhase) {
    ClockSourceMock clock;
    SpyingDocumentUpdater updater;
    updater.getOnDiskStateForModification().setState(CoordinatorStateEnum::kApplying);
    ReshardingCoordinatorDao dao;
    OperationContext* opCtx = nullptr;

    auto uuid = UUID::gen();
    auto now = clock.now();
    auto criticalSectionExpiresAt = now + Seconds(5);
    dao.transitionToBlockingWritesPhase(opCtx, &updater, now, criticalSectionExpiresAt, uuid);
    const auto& lastRequest = updater.getLastRequest();

    auto expectedUpdates = BSON_ARRAY(BSON(
        "q" << BSON("_id" << uuid) << "u"
            << BSON("$set" << BSON("state" << "blocking-writes"
                                           << "criticalSectionExpiresAt" << criticalSectionExpiresAt
                                           << "metrics.oplogApplication.stop" << now))
            << "multi" << false << "upsert" << false));

    ASSERT_EQUALS(lastRequest.getStringField("update"),
                  NamespaceString::kConfigReshardingOperationsNamespace.coll());
    auto updates = lastRequest.getObjectField("updates");
    ASSERT_BSONOBJ_EQ_UNORDERED(updates, expectedUpdates);
}

DEATH_TEST(ReshardingCoordinatorDaoTest,
           TransitionToBlockingWritesPhasePreviousStateInvariant,
           "invariant") {
    ClockSourceMock clock;
    SpyingDocumentUpdater updater;
    updater.getOnDiskStateForModification().setState(CoordinatorStateEnum::kAborting);
    ReshardingCoordinatorDao dao;
    OperationContext* opCtx = nullptr;

    auto uuid = UUID::gen();
    auto now = clock.now();
    auto criticalSectionExpiresAt = now + Seconds(5);
    dao.transitionToBlockingWritesPhase(opCtx, &updater, now, criticalSectionExpiresAt, uuid);
}

}  // namespace resharding
}  // namespace mongo
