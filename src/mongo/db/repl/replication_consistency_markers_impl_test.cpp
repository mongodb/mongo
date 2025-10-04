/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_consistency_markers_impl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

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

/**
 * Returns min valid document.
 */
BSONObj getMinValidDocument(OperationContext* opCtx, const NamespaceString& minValidNss) {
    return writeConflictRetry(opCtx, "getMinValidDocument", minValidNss, [opCtx, minValidNss] {
        Lock::DBLock dblk(opCtx, minValidNss.dbName(), MODE_IS);
        Lock::CollectionLock lk(opCtx, minValidNss, MODE_IS);
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
    ReplicationConsistencyMarkersTest()
        : ServiceContextMongoDTest(Options{}.useJournalListener(
              std::make_unique<JournalListenerWithDurabilityTracking>())) {}

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

TEST_F(ReplicationConsistencyMarkersTest, InitialSyncFlag) {
    ReplicationConsistencyMarkersImpl consistencyMarkers(
        getStorageInterface(), kMinValidNss, kOplogTruncateAfterPointNss, kInitialSyncIdNss);
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
    ReplicationConsistencyMarkersImpl consistencyMarkers(
        getStorageInterface(), kMinValidNss, kOplogTruncateAfterPointNss, kInitialSyncIdNss);
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
    ReplicationConsistencyMarkersImpl consistencyMarkers(
        getStorageInterface(), kMinValidNss, kOplogTruncateAfterPointNss, kInitialSyncIdNss);
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
    ReplicationConsistencyMarkersImpl consistencyMarkers(
        getStorageInterface(), kMinValidNss, kOplogTruncateAfterPointNss, kInitialSyncIdNss);
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
    ReplicationConsistencyMarkersImpl consistencyMarkers(
        getStorageInterface(), kMinValidNss, kOplogTruncateAfterPointNss, kInitialSyncIdNss);
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

}  // namespace
}  // namespace mongo
