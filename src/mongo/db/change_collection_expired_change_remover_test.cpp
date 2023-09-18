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

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ChangeCollectionExpiredChangeRemoverTest : public CatalogTestFixture {
protected:
    ChangeCollectionExpiredChangeRemoverTest()
        : CatalogTestFixture(Options{}.useMockClock(true)),
          _tenantId(change_stream_serverless_helpers::getTenantIdForTesting()) {
        ChangeStreamChangeCollectionManager::create(getServiceContext());
    }

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    Date_t now() {
        return clockSource()->now();
    }

    Timestamp insertDocumentToChangeCollection(OperationContext* opCtx,
                                               const TenantId& tenantId,
                                               const BSONObj& obj) {
        AutoGetChangeCollection changeCollection{
            opCtx, AutoGetChangeCollection::AccessMode::kWrite, tenantId};

        WriteUnitOfWork wunit(opCtx);

        const auto wallTime = now();
        const auto opTime = repl::getNextOpTime(opCtx);
        const Timestamp timestamp = opTime.getTimestamp();

        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpTime(opTime);
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(NamespaceString::makeChangeCollectionNSS(tenantId));
        oplogEntry.setObject(obj);
        oplogEntry.setWallClockTime(wallTime);

        auto oplogEntryBson = oplogEntry.toBSON();

        RecordData recordData{oplogEntryBson.objdata(), oplogEntryBson.objsize()};
        RecordId recordId =
            record_id_helpers::keyForOptime(timestamp, KeyFormat::String).getValue();
        ChangeStreamChangeCollectionManager::get(opCtx).insertDocumentsToChangeCollection(
            opCtx, {Record{recordId, recordData}}, {timestamp});
        wunit.commit();

        return timestamp;
    }

    std::vector<repl::OplogEntry> readChangeCollection(OperationContext* opCtx,
                                                       const TenantId& tenantId) {
        auto changeCollection =
            AutoGetChangeCollection{opCtx, AutoGetChangeCollection::AccessMode::kRead, tenantId};

        auto scanExecutor =
            InternalPlanner::collectionScan(opCtx,
                                            &(*changeCollection),
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            InternalPlanner::Direction::FORWARD);

        BSONObj currChangeDoc;
        std::vector<repl::OplogEntry> entries;
        while (scanExecutor->getNext(&currChangeDoc, nullptr) == PlanExecutor::ADVANCED) {
            entries.push_back(repl::OplogEntry(currChangeDoc));
        }

        return entries;
    }

    void dropAndRecreateChangeCollection(OperationContext* opCtx, const TenantId& tenantId) {
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx);
        changeCollectionManager.dropChangeCollection(opCtx, tenantId);
        changeCollectionManager.createChangeCollection(opCtx, tenantId);
    }

    size_t removeExpiredChangeCollectionsDocuments(OperationContext* opCtx,
                                                   const TenantId& tenantId,
                                                   Date_t expirationTime) {
        // Acquire intent-exclusive lock on the change collection. Early exit if the collection
        // doesn't exist.
        const auto changeCollection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::makeChangeCollectionNSS(tenantId),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);

        // Get the 'maxRecordIdBound' and perform the removal of the expired documents.
        const auto maxRecordIdBound =
            ChangeStreamChangeCollectionManager::getChangeCollectionPurgingJobMetadata(
                opCtx, changeCollection)
                ->maxRecordIdBound;
        return ChangeStreamChangeCollectionManager::
            removeExpiredChangeCollectionsDocumentsWithCollScan(
                opCtx, changeCollection, maxRecordIdBound, expirationTime);
    }

    const TenantId _tenantId;
    boost::optional<ChangeStreamChangeCollectionManager> _changeCollectionManager;

    RAIIServerParameterControllerForTest featureFlagController{"featureFlagServerlessChangeStreams",
                                                               true};
    RAIIServerParameterControllerForTest queryKnobController{
        "internalChangeStreamUseTenantIdForTesting", true};
};

class ChangeCollectionTruncateExpirationTest : public ChangeCollectionExpiredChangeRemoverTest {
protected:
    ChangeCollectionTruncateExpirationTest() : ChangeCollectionExpiredChangeRemoverTest() {}

    // Forces the 'lastApplied' Timestamp to be 'targetTimestamp'. The ReplicationCoordinator keeps
    // track of OpTimeAndWallTime for 'lastApplied'. This method exclusively changes the
    // 'opTime.timestamp', but not the other values (term, wallTime, etc) associated with
    // 'lastApplied'.
    void forceLastAppliedTimestamp(OperationContext* opCtx, Timestamp targetTimestamp) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto lastAppliedOpTimeAndWallTime = replCoord->getMyLastAppliedOpTimeAndWallTime();
        auto newOpTime =
            repl::OpTime(targetTimestamp, lastAppliedOpTimeAndWallTime.opTime.getTerm());
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            repl::OpTimeAndWallTime(newOpTime, lastAppliedOpTimeAndWallTime.wallTime));

        // Verify the Timestamp is set accordingly.
        ASSERT_EQ(replCoord->getMyLastAppliedOpTimeAndWallTime().opTime.getTimestamp(),
                  targetTimestamp);
    }

    void setExpireAfterSeconds(OperationContext* opCtx, Seconds seconds) {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        auto* changeStreamsParam =
            clusterParameters
                ->get<ClusterParameterWithStorage<ChangeStreamsClusterParameterStorage>>(
                    "changeStreams");

        auto oldSettings = changeStreamsParam->getValue(_tenantId);
        oldSettings.setExpireAfterSeconds(seconds.count());
        changeStreamsParam->setValue(oldSettings, _tenantId).ignore();
    }

    size_t removeExpiredChangeCollectionsDocuments(OperationContext* opCtx,
                                                   const TenantId& tenantId,
                                                   Date_t expirationTime) {
        // Acquire intent-exclusive lock on the change collection. Early exit if the collection
        // doesn't exist.
        const auto changeCollection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::makeChangeCollectionNSS(tenantId),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);

        return ChangeStreamChangeCollectionManager::
            removeExpiredChangeCollectionsDocumentsWithTruncate(
                opCtx, changeCollection, expirationTime);
    }

    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagUseUnreplicatedTruncatesForDeletions", true};
};

// Tests that the last expired focument retrieved is the expected one.
TEST_F(ChangeCollectionExpiredChangeRemoverTest, VerifyLastExpiredDocument) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    Date_t expirationTime;
    BSONObj lastExpiredDocument;

    // Create 100 change documents and consider the first 50 of them as expired.
    for (int i = 0; i < 100; ++i) {
        auto doc = BSON("_id" << i);
        insertDocumentToChangeCollection(opCtx, _tenantId, doc);

        // Store the last document and it's wallTime.
        if (i == 99) {
            lastExpiredDocument = doc;
            expirationTime = now();
        }

        clockSource()->advance(Milliseconds(1));
    }

    const auto changeCollection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::makeChangeCollectionNSS(_tenantId),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);

    auto maxExpiredRecordId =
        ChangeStreamChangeCollectionManager::getChangeCollectionPurgingJobMetadata(opCtx,
                                                                                   changeCollection)
            ->maxRecordIdBound;

    // Get the document found at 'maxExpiredRecordId' and test it against 'lastExpiredDocument'.
    auto scanExecutor =
        InternalPlanner::collectionScan(opCtx,
                                        changeCollection,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        InternalPlanner::Direction::FORWARD,
                                        boost::none,
                                        maxExpiredRecordId,
                                        maxExpiredRecordId);

    BSONObj changeDocAtId;
    ASSERT_EQ(scanExecutor->getNext(&changeDocAtId, nullptr), PlanExecutor::ADVANCED);
    ASSERT_BSONOBJ_EQ(repl::OplogEntry(changeDocAtId).getObject(), lastExpiredDocument);
}

// Tests that only the expired documents are removed from the change collection.
TEST_F(ChangeCollectionExpiredChangeRemoverTest, ShouldRemoveOnlyExpiredDocument) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    const BSONObj firstExpired = BSON("_id"
                                      << "firstExpired");
    const BSONObj secondExpired = BSON("_id"
                                       << "secondExpired");
    const BSONObj notExpired = BSON("_id"
                                    << "notExpired");

    insertDocumentToChangeCollection(opCtx, _tenantId, firstExpired);
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, secondExpired);

    // Store the wallTime of the last expired document.
    const auto expirationTime = now();
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, notExpired);

    // Verify that only the required documents are removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, expirationTime), 2);

    // Only the 'notExpired' document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), notExpired);
}

// Tests that the last expired document is never deleted.
TEST_F(ChangeCollectionExpiredChangeRemoverTest, ShouldLeaveAtLeastOneDocument) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    for (int i = 0; i < 100; ++i) {
        insertDocumentToChangeCollection(opCtx, _tenantId, BSON("_id" << i));
        clockSource()->advance(Milliseconds{1});
    }

    // Verify that all but the last document is removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, now()), 99);

    // Only the last document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), BSON("_id" << 99));
}

// Tests that only the expired documents are removed from the change collection.
TEST_F(ChangeCollectionTruncateExpirationTest, ShouldRemoveOnlyExpiredDocument_Markers) {
    const BSONObj firstExpired = BSON("_id"
                                      << "firstExpired");
    const BSONObj secondExpired = BSON("_id"
                                       << "secondExpired");
    const BSONObj notExpired = BSON("_id"
                                    << "notExpired");

    RAIIServerParameterControllerForTest minBytesPerMarker{
        "changeCollectionTruncateMarkersMinBytes",
        firstExpired.objsize() + secondExpired.objsize()};

    const auto timeAtStart = now();
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    insertDocumentToChangeCollection(opCtx, _tenantId, firstExpired);
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, secondExpired);

    // Store the wallTime of the last expired document.
    const auto expirationTime = now();
    const auto expirationTimeInSeconds = duration_cast<Seconds>(expirationTime - timeAtStart);
    setExpireAfterSeconds(opCtx, expirationTimeInSeconds);
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, notExpired);

    const auto allDurableTimestamp =
        storageInterface()->getAllDurableTimestamp(getServiceContext());
    forceLastAppliedTimestamp(opCtx, allDurableTimestamp);

    // Verify that only the required documents are removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, expirationTime), 2);

    // Only the 'notExpired' document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), notExpired);
}

// Tests that the last expired document is never deleted.
TEST_F(ChangeCollectionTruncateExpirationTest, ShouldLeaveAtLeastOneDocument_Markers) {
    RAIIServerParameterControllerForTest minBytesPerMarker{
        "changeCollectionTruncateMarkersMinBytes", 1};
    const auto opCtx = operationContext();

    dropAndRecreateChangeCollection(opCtx, _tenantId);

    setExpireAfterSeconds(opCtx, Seconds{1});

    for (int i = 0; i < 100; ++i) {
        insertDocumentToChangeCollection(opCtx, _tenantId, BSON("_id" << i));
        clockSource()->advance(Seconds{1});
    }

    const auto allDurableTimestamp =
        storageInterface()->getAllDurableTimestamp(getServiceContext());
    forceLastAppliedTimestamp(opCtx, allDurableTimestamp);

    // Verify that all but the last document is removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, now()), 99);

    // Only the last document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), BSON("_id" << 99));
}

TEST_F(ChangeCollectionTruncateExpirationTest, TruncatesAreOnlyAfterAllDurable) {
    RAIIServerParameterControllerForTest minBytesPerMarker{
        "changeCollectionTruncateMarkersMinBytes", 1};
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    setExpireAfterSeconds(opCtx, Seconds{1});

    int lastId = 0;
    for (int i = 0; i < 100; ++i) {
        insertDocumentToChangeCollection(opCtx, _tenantId, BSON("_id" << lastId++));
        clockSource()->advance(Seconds{1});
    }

    // Create an oplog hole in an alternate client.
    auto altClient = getServiceContext()->makeClient("alt-client");
    auto altOpCtx = altClient->makeOperationContext();

    // Wrap insertDocumentToChangeCollection call in WUOW to prevent inner WUOW from commiting.
    WriteUnitOfWork wuow(altOpCtx.get());
    const auto tsAtHole =
        insertDocumentToChangeCollection(altOpCtx.get(), _tenantId, BSON("_id" << lastId++));
    clockSource()->advance(Seconds{1});

    ASSERT_LT(storageInterface()->getAllDurableTimestamp(getServiceContext()), tsAtHole);

    // Insert 20 additional documents after the hole, which should not be truncated.
    for (int i = 0; i < 20; ++i) {
        insertDocumentToChangeCollection(opCtx, _tenantId, BSON("_id" << lastId++));
        clockSource()->advance(Seconds{1});
    }

    const auto allDurableTSPostHoleInserts =
        storageInterface()->getAllDurableTimestamp(getServiceContext());
    ASSERT_LT(allDurableTSPostHoleInserts, tsAtHole);
    // Set a lastApplied > allDurable.
    forceLastAppliedTimestamp(opCtx, Timestamp(allDurableTSPostHoleInserts.getSecs() + 1, 0));

    // Verify that all documents before all_durable have been deleted.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, now()), 100);

    wuow.commit();

    // The documents added after the oplog hole have not been truncated.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 21);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), BSON("_id" << 100));
}

TEST_F(ChangeCollectionTruncateExpirationTest, StartupRecoveryTruncates) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    clockSource()->advance(Seconds(100));

    const auto numEntries = 20;
    for (int i = 0; i < numEntries; ++i) {
        auto doc = BSON("_id" << i);
        insertDocumentToChangeCollection(opCtx, _tenantId, doc);
        clockSource()->advance(Seconds(1));
    }

    // Only the first entry is expired by the current wall time.
    setExpireAfterSeconds(opCtx, Seconds(numEntries));

    auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), numEntries);

    startup_recovery::recoverChangeStreamCollections(
        opCtx, false, StorageEngine::LastShutdownState::kUnclean);

    // All entries within 'startup_recovery::kChangeStreamPostUncleanShutdownExpiryExtensionSeconds'
    // seconds of expiry should be truncated.
    changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_LT(static_cast<int64_t>(changeCollectionEntries.size()),
              numEntries -
                  startup_recovery::kChangeStreamPostUncleanShutdownExpiryExtensionSeconds);
}
}  // namespace mongo
