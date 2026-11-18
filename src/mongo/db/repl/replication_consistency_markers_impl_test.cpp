// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/replication_consistency_markers_impl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <array>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

using namespace mongo::repl;

NamespaceString kMinValidNss = NamespaceString::kDefaultMinValidNamespace;
NamespaceString kOplogTruncateAfterPointNss =
    NamespaceString::createNamespaceString_forTest("local", "replset.oplogTruncateAfterPoint");
NamespaceString kInitialSyncIdNss = NamespaceString::kDefaultInitialSyncIdNamespace;
NamespaceString kRecordIdsGroupIdNss = NamespaceString::kDefaultRecordIdsGroupIdNamespace;

/**
 * Returns min valid document.
 */
BSONObj getMinValidDocument(OperationContext* opCtx, const NamespaceString& minValidNss) {
    return writeConflictRetry(opCtx, "getMinValidDocument", minValidNss, [opCtx, minValidNss] {
        auto acq = acquireCollection(opCtx,
                                     CollectionAcquisitionRequest::fromOpCtx(
                                         opCtx, minValidNss, AcquisitionPrerequisites::kRead),
                                     MODE_IS);
        BSONObj mv;
        if (Helpers::getSingleton(opCtx, minValidNss, mv)) {
            return mv;
        }
        return mv;
    });
}

class JournalListenerWithDurabilityTracking : public JournalListener {
public:
    std::unique_ptr<Token> getToken(OperationContext* opCtx) override {
        return {};
    }

    void onDurable(const Token& token) override {
        onDurableCalled = true;
    }

    bool onDurableCalled = false;
};

class ReplicationConsistencyMarkersTest : public ServiceContextMongoDTest {
protected:
    Options makeServiceContextOptions() override {
        return ServiceContextMongoDTest::makeServiceContextOptions().useJournalListener(
            std::make_unique<JournalListenerWithDurabilityTracking>());
    }

    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    StorageInterface* getStorageInterface() {
        return _storageInterface.get();
    }

    JournalListenerWithDurabilityTracking* getJournalListener() {
        return static_cast<JournalListenerWithDurabilityTracking*>(journalListener());
    }

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _createOpCtx();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(getServiceContext());
        ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
        _storageInterface = std::make_unique<StorageInterfaceImpl>();
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        _storageInterface.reset();
        ServiceContextMongoDTest::tearDown();
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<StorageInterfaceImpl> _storageInterface;
};

// Death tests must live in their own suite: DEATH_TEST_F wraps the fixture in a different
// generated class, and Google Test requires all tests in a suite to share one fixture class.
class ReplicationConsistencyMarkersDeathTest : public ReplicationConsistencyMarkersTest {};

TEST_F(ReplicationConsistencyMarkersTest, InitialSyncFlag) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    ASSERT(consistencyMarkers.createInternalCollections(opCtx).isOK());
    consistencyMarkers.initializeMinValidDocument(opCtx);

    // Initial sync flag should be unset after initializing a new storage engine.
    ASSERT_FALSE(consistencyMarkers.getInitialSyncFlag(opCtx));

    // Setting initial sync flag should affect getInitialSyncFlag() result.
    consistencyMarkers.setInitialSyncFlag(opCtx);
    ASSERT_TRUE(consistencyMarkers.getInitialSyncFlag(opCtx));

    // Check min valid document using storage engine interface.
    auto minValidDocument = getMinValidDocument(opCtx, kMinValidNss);
    ASSERT_TRUE(minValidDocument.hasField(MinValidDocument::kInitialSyncFlagFieldName));
    ASSERT_TRUE(minValidDocument.getBoolField(MinValidDocument::kInitialSyncFlagFieldName));

    // Clearing initial sync flag should affect getInitialSyncFlag() result.
    consistencyMarkers.clearInitialSyncFlag(opCtx);
    ASSERT_FALSE(consistencyMarkers.getInitialSyncFlag(opCtx));
}

TEST_F(ReplicationConsistencyMarkersTest, GetMarkersAfterSettingInitialSyncFlagWorks) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    ASSERT(consistencyMarkers.createInternalCollections(opCtx).isOK());
    consistencyMarkers.initializeMinValidDocument(opCtx);

    // Initial sync flag should be unset after initializing a new storage engine.
    ASSERT_FALSE(consistencyMarkers.getInitialSyncFlag(opCtx));

    // Setting initial sync flag should affect getInitialSyncFlag() result.
    consistencyMarkers.setInitialSyncFlag(opCtx);
    ASSERT_TRUE(consistencyMarkers.getInitialSyncFlag(opCtx));

    ASSERT(consistencyMarkers.getAppliedThrough(opCtx).isNull());
    ASSERT(consistencyMarkers.getOplogTruncateAfterPoint(opCtx).isNull());
}

TEST_F(ReplicationConsistencyMarkersTest, ClearInitialSyncFlagResetsOplogTruncateAfterPoint) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    ASSERT(consistencyMarkers.createInternalCollections(opCtx).isOK());
    consistencyMarkers.initializeMinValidDocument(opCtx);

    ASSERT(consistencyMarkers.getOplogTruncateAfterPoint(opCtx).isNull());
    ASSERT_FALSE(consistencyMarkers.getInitialSyncFlag(opCtx));

    // Set the oplog truncate after point and verify it has been set correctly.
    OpTime endOpTime({Seconds(456), 0}, 1LL);
    consistencyMarkers.setOplogTruncateAfterPoint(opCtx, endOpTime.getTimestamp());
    ASSERT_EQ(consistencyMarkers.getOplogTruncateAfterPoint(opCtx), endOpTime.getTimestamp());

    // Clear the initial sync flag.
    consistencyMarkers.clearInitialSyncFlag(opCtx);
    ASSERT_FALSE(consistencyMarkers.getInitialSyncFlag(opCtx));

    // Make sure the oplog truncate after point no longer exists.
    ASSERT(consistencyMarkers.getOplogTruncateAfterPoint(opCtx).isNull());
}

TEST_F(ReplicationConsistencyMarkersTest, ReplicationConsistencyMarkers) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    ASSERT(consistencyMarkers.createInternalCollections(opCtx).isOK());
    consistencyMarkers.initializeMinValidDocument(opCtx);

    // MinValid boundaries should all be null after initializing a new storage engine.
    ASSERT(consistencyMarkers.getAppliedThrough(opCtx).isNull());
    ASSERT(consistencyMarkers.getOplogTruncateAfterPoint(opCtx).isNull());

    // Setting min valid boundaries should affect get*() result.
    OpTime startOpTime({Seconds(123), 0}, 1LL);
    OpTime endOpTime({Seconds(456), 0}, 1LL);
    consistencyMarkers.setAppliedThrough(opCtx, startOpTime);
    consistencyMarkers.setOplogTruncateAfterPoint(opCtx, endOpTime.getTimestamp());

    ASSERT_EQ(consistencyMarkers.getAppliedThrough(opCtx), startOpTime);
    ASSERT_EQ(consistencyMarkers.getOplogTruncateAfterPoint(opCtx), endOpTime.getTimestamp());

    // Check min valid document using storage engine interface.
    auto minValidDocument = getMinValidDocument(opCtx, kMinValidNss);
    ASSERT_TRUE(minValidDocument.hasField(MinValidDocument::kAppliedThroughFieldName));
    ASSERT_TRUE(minValidDocument[MinValidDocument::kAppliedThroughFieldName].isABSONObj());
    ASSERT_EQUALS(startOpTime,
                  unittest::assertGet(OpTime::parseFromOplogEntry(
                      minValidDocument[MinValidDocument::kAppliedThroughFieldName].Obj())));

    // Check oplog truncate after point document.
    ASSERT_EQUALS(endOpTime.getTimestamp(), consistencyMarkers.getOplogTruncateAfterPoint(opCtx));
}

TEST_F(ReplicationConsistencyMarkersTest, InitialSyncId) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();

    // Initially, initialSyncId should be unset.
    auto initialSyncIdShouldBeUnset = consistencyMarkers.getInitialSyncId(opCtx);
    ASSERT(initialSyncIdShouldBeUnset.isEmpty()) << initialSyncIdShouldBeUnset;

    // Clearing an already-clear initialSyncId should be OK.
    consistencyMarkers.clearInitialSyncId(opCtx);
    initialSyncIdShouldBeUnset = consistencyMarkers.getInitialSyncId(opCtx);
    ASSERT(initialSyncIdShouldBeUnset.isEmpty()) << initialSyncIdShouldBeUnset;

    consistencyMarkers.setInitialSyncIdIfNotSet(opCtx);
    auto firstInitialSyncIdBson = consistencyMarkers.getInitialSyncId(opCtx);
    ASSERT_FALSE(firstInitialSyncIdBson.isEmpty());
    InitialSyncIdDocument firstInitialSyncIdDoc =
        InitialSyncIdDocument::parse(firstInitialSyncIdBson, IDLParserContext("initialSyncId"));

    // Setting it twice should change nothing.
    consistencyMarkers.setInitialSyncIdIfNotSet(opCtx);
    ASSERT_BSONOBJ_EQ(firstInitialSyncIdBson, consistencyMarkers.getInitialSyncId(opCtx));

    // Clear it; should return to empty.
    consistencyMarkers.clearInitialSyncId(opCtx);
    initialSyncIdShouldBeUnset = consistencyMarkers.getInitialSyncId(opCtx);
    ASSERT(initialSyncIdShouldBeUnset.isEmpty()) << initialSyncIdShouldBeUnset;

    // Set it; it should have a different UUID.
    consistencyMarkers.setInitialSyncIdIfNotSet(opCtx);
    auto secondInitialSyncIdBson = consistencyMarkers.getInitialSyncId(opCtx);
    ASSERT_FALSE(secondInitialSyncIdBson.isEmpty());
    InitialSyncIdDocument secondInitialSyncIdDoc =
        InitialSyncIdDocument::parse(secondInitialSyncIdBson, IDLParserContext("initialSyncId"));
    ASSERT_NE(firstInitialSyncIdDoc.get_id(), secondInitialSyncIdDoc.get_id());
}

TEST_F(ReplicationConsistencyMarkersTest, RecordIdsGroupIdLifecycle) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();

    // Initially the node holds no groupId: neither the collection nor the document exist.
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));

    // Clearing an already-clear groupId is a no-op. The WriteUnitOfWork stands in for the
    // fencing entry's. Callers without a fencing entry open their own around the call.
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.clearRecordIdsGroupId(opCtx);
        wuow.commit();
    }
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));

    // Creating the collection alone does not make the node a group member.
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));

    // Creating the collection again is a no-op.
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));

    // Mint a groupId. The WriteUnitOfWork stands in for the fencing entry's, and the
    // foundingOpTime for the fencing entry's OpTime.
    auto groupId = UUID::gen();
    OpTime foundingOpTime({Seconds(123), 4}, 1LL);
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, groupId, foundingOpTime);
        wuow.commit();
    }
    auto doc = consistencyMarkers.getRecordIdsGroupId(opCtx);
    ASSERT_TRUE(doc);
    ASSERT_EQ(doc->get_id(), kRecordIdsGroupIdDocId);
    ASSERT_EQ(doc->getGroupId(), groupId);
    ASSERT_EQ(doc->getFoundingOpTime(), foundingOpTime);

    // Clearing drops the collection: the node is no longer a group member.
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.clearRecordIdsGroupId(opCtx);
        wuow.commit();
    }
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));

    // A new groupId can be minted after a clear.
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, groupId, foundingOpTime);
        wuow.commit();
    }
    doc = consistencyMarkers.getRecordIdsGroupId(opCtx);
    ASSERT_TRUE(doc);
    ASSERT_EQ(doc->getGroupId(), groupId);
    ASSERT_EQ(doc->getFoundingOpTime(), foundingOpTime);
}

TEST_F(ReplicationConsistencyMarkersTest, EffectiveRecordIdsGroupIdCacheCoherence) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();

    // After the startup load, a groupless node is not a member.
    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));

    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));

    auto groupId = UUID::gen();
    OpTime foundingOpTime({Seconds(123), 4}, 1LL);

    // An aborted mint leaves both the marker and the cache groupless.
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, groupId, foundingOpTime);
        ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
    }
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));

    // A committed mint is published to the cache only at commit.
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, groupId, foundingOpTime);
        ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
        wuow.commit();
    }
    auto membership = consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx);
    ASSERT_TRUE(membership);
    ASSERT_EQ(membership->groupId, groupId);
    ASSERT_EQ(membership->foundingOpTime, foundingOpTime);

    // An aborted clear leaves both the marker and the cache intact.
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.clearRecordIdsGroupId(opCtx);
        ASSERT_TRUE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
    }
    ASSERT_TRUE(consistencyMarkers.getRecordIdsGroupId(opCtx));
    ASSERT_TRUE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));

    // A committed clear is published to the cache only at commit.
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.clearRecordIdsGroupId(opCtx);
        ASSERT_TRUE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
        wuow.commit();
    }
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));

    // The cache always agrees with what a fresh load from storage produces.
    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
}

TEST_F(ReplicationConsistencyMarkersTest, EffectiveRecordIdsGroupIdLoadsPersistedMarker) {
    auto opCtx = getOperationContext();
    auto groupId = UUID::gen();
    OpTime foundingOpTime({Seconds(123), 4}, 1LL);
    {
        ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                             kMinValidNss,
                                                             kOplogTruncateAfterPointNss,
                                                             kInitialSyncIdNss,
                                                             kRecordIdsGroupIdNss);
        consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, groupId, foundingOpTime);
        wuow.commit();
    }

    // Simulate a restart: a fresh instance starts with an unloaded cache while the marker is
    // persisted, and the startup load picks it up.
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);

    // The fresh instance's cache is unloaded, so a read before the startup load trips the tassert
    // rather than reporting "not a member".
    ASSERT_TASSERT_CODE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx), 13843302);

    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    auto membership = consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx);
    ASSERT_TRUE(membership);
    ASSERT_EQ(membership->groupId, groupId);
    ASSERT_EQ(membership->foundingOpTime, foundingOpTime);
}

TEST_F(ReplicationConsistencyMarkersTest, EffectiveRecordIdsGroupIdDoesNotReadStorage) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    auto groupId = UUID::gen();
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, groupId, OpTime({Seconds(123), 4}, 1LL));
        wuow.commit();
    }

    // Drop the marker collection behind the cache's back. If the accessor consulted storage it
    // would now report no membership; it must keep serving the cached value.
    ASSERT_OK(getStorageInterface()->dropCollection(opCtx, kRecordIdsGroupIdNss));
    ASSERT_FALSE(consistencyMarkers.getRecordIdsGroupId(opCtx));
    auto membership = consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx);
    ASSERT_TRUE(membership);
    ASSERT_EQ(membership->groupId, groupId);

    // Changes underneath the cache are only picked up by an explicit load (as rollback does after
    // recovering to the stable timestamp).
    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
}

TEST_F(ReplicationConsistencyMarkersTest, EffectiveRecordIdsGroupIdCachesNonMembership) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();

    // "Not a member" is cached too: a marker written behind the cache's back is not seen until
    // the next load.
    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));
    auto groupId = UUID::gen();
    {
        ReplicationConsistencyMarkersImpl otherMarkers(getStorageInterface(),
                                                       kMinValidNss,
                                                       kOplogTruncateAfterPointNss,
                                                       kInitialSyncIdNss,
                                                       kRecordIdsGroupIdNss);
        otherMarkers.createRecordIdsGroupIdCollection(opCtx);
        WriteUnitOfWork wuow(opCtx);
        otherMarkers.setRecordIdsGroupId(opCtx, groupId, OpTime({Seconds(123), 4}, 1LL));
        wuow.commit();
    }
    ASSERT_FALSE(consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx));

    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    auto membership = consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx);
    ASSERT_TRUE(membership);
    ASSERT_EQ(membership->groupId, groupId);
}

/**
 * Attached storage in every respect except that it mandates replicated RecordIds.
 */
class ReplicatedRecordIdsMandatedPersistenceProvider : public rss::AttachedPersistenceProvider {
public:
    bool shouldUseReplicatedRecordIds() const override {
        return true;
    }
};

class ReplicationConsistencyMarkersReplicatedRecordIdsProviderTest
    : public ReplicationConsistencyMarkersTest {
protected:
    Options makeServiceContextOptions() override {
        return ReplicationConsistencyMarkersTest::makeServiceContextOptions()
            .setPersistenceProvider(
                std::make_unique<ReplicatedRecordIdsMandatedPersistenceProvider>());
    }
};

TEST_F(ReplicationConsistencyMarkersReplicatedRecordIdsProviderTest,
       EffectiveRecordIdsGroupIdIsSentinel) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();

    auto assertIsAllZero = [&] {
        auto membership = consistencyMarkers.getEffectiveRecordIdsGroupId(opCtx);
        ASSERT_TRUE(membership);
        ASSERT_EQ(membership->groupId.data(), (std::array<unsigned char, UUID::kNumBytes>{}));
        ASSERT_EQ(membership->foundingOpTime.getTimestamp(), Timestamp(0, 0));
        ASSERT_EQ(membership->foundingOpTime.getTerm(), OpTime::kUninitializedTerm);
    };

    // The sentinel is reported without any load: when the provider mandates replicated
    // RecordIds, the cached marker is never consulted.
    assertIsAllZero();
    consistencyMarkers.reloadEffectiveRecordIdsGroupId(opCtx);
    assertIsAllZero();

    // Even if a marker were present, the provider mandate wins.
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, UUID::gen(), OpTime({Seconds(123), 4}, 1LL));
        wuow.commit();
    }
    assertIsAllZero();
}

DEATH_TEST_F(ReplicationConsistencyMarkersDeathTest,
             CreateRecordIdsGroupIdCollectionRejectsActiveWriteUnitOfWork,
             "createRecordIdsGroupIdCollection manages its own WriteUnitOfWork") {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();

    // Creation is an untimestamped catalog operation and must run before the fencing entry's
    // WriteUnitOfWork opens, never inside it.
    WriteUnitOfWork wuow(opCtx);
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
}

DEATH_TEST_F(ReplicationConsistencyMarkersDeathTest,
             SetRecordIdsGroupIdRequiresWriteUnitOfWork,
             "setRecordIdsGroupId must be called within") {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);

    // Document writes to the recordIdsGroupId collection must always be timestamped, so a write
    // outside of the fencing entry's WriteUnitOfWork is a programming error.
    consistencyMarkers.setRecordIdsGroupId(opCtx, UUID::gen(), OpTime({Seconds(123), 4}, 1LL));
}

DEATH_TEST_F(ReplicationConsistencyMarkersDeathTest,
             ClearRecordIdsGroupIdRequiresWriteUnitOfWork,
             "clearRecordIdsGroupId must be called within") {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);

    // The clear must commit atomically with the fencing entry, so calling it outside of a
    // WriteUnitOfWork is a programming error.
    consistencyMarkers.clearRecordIdsGroupId(opCtx);
}

DEATH_TEST_F(ReplicationConsistencyMarkersDeathTest,
             SetRecordIdsGroupIdFailsWhenAlreadySet,
             "must not overwrite an existing recordIdsGroupId document") {
    ReplicationConsistencyMarkersImpl consistencyMarkers(getStorageInterface(),
                                                         kMinValidNss,
                                                         kOplogTruncateAfterPointNss,
                                                         kInitialSyncIdNss,
                                                         kRecordIdsGroupIdNss);
    auto opCtx = getOperationContext();
    consistencyMarkers.createRecordIdsGroupIdCollection(opCtx);
    {
        WriteUnitOfWork wuow(opCtx);
        consistencyMarkers.setRecordIdsGroupId(opCtx, UUID::gen(), OpTime({Seconds(123), 4}, 1LL));
        wuow.commit();
    }

    // A groupId is only ever written while groupless. Writing over an existing document would
    // silently overwrite the membership marker, so it fails instead.
    WriteUnitOfWork wuow(opCtx);
    consistencyMarkers.setRecordIdsGroupId(opCtx, UUID::gen(), OpTime({Seconds(567), 8}, 1LL));
}

}  // namespace
}  // namespace mongo
