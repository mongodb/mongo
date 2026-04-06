/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_op_observer.h"

#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Test fixture for ReplicatedFastCountOpObserver unit tests. Sets up the full environment needed
 * to exercise onCreateCollection and onDropCollection, including a mock replica set primary, the
 * internal fast count collection, and the observer registered in the op observer registry.
 */
class ReplicatedFastCountOpObserverTest : public CatalogTestFixture {
public:
    ReplicatedFastCountOpObserverTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count_test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();

        auto* replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(getServiceContext()));
        ASSERT(replCoord);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        auto* registry = dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        ASSERT(registry);
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        // Disable the background periodic write thread before startup so it never starts running.
        // This prevents it from leaving an open client when the service context is torn down.
        ReplicatedFastCountManager::get(_opCtx->getServiceContext())
            .disablePeriodicWrites_ForTest();

        setUpReplicatedFastCount(_opCtx);

        registry->addObserver(
            std::make_unique<replicated_fast_count::ReplicatedFastCountOpObserver>());
    }

    OperationContext* _opCtx;
};


TEST_F(ReplicatedFastCountOpObserverTest, CollectionCreationAddsEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    NamespaceString newNss = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "newlyCreatedCollection");

    ASSERT_OK(createCollection(_opCtx, newNss.dbName(), BSON("create" << newNss.coll())));

    UUID newCollUUID = UUID::gen();
    {
        AutoGetCollection coll(_opCtx, newNss, LockMode::MODE_IS);
        newCollUUID = coll->uuid();
    }

    // The op observer should have written the entry directly to disk with count=0, size=0.
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, newCollUUID, /*expectPersisted=*/true, /*expectedCount=*/0, /*expectedSize=*/0);

    auto entry = replicated_fast_count::SizeCountStore{}.read(_opCtx, newCollUUID);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->timestamp, Timestamp(0, 0));
    EXPECT_EQ(entry->count, 0);
    EXPECT_EQ(entry->size, 0);
}

TEST_F(ReplicatedFastCountOpObserverTest, CollectionCreationOnSecondaryAddsEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto* replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));
    ASSERT(replCoord);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));

    const UUID uuid = UUID::gen();
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "newColl");

    CollectionOptions options;
    options.uuid = uuid;

    replicated_fast_count::ReplicatedFastCountOpObserver observer;

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx);
        WriteUnitOfWork wuow(_opCtx);
        observer.onCreateCollection(_opCtx,
                                    nss,
                                    options,
                                    /*idIndex=*/BSONObj{},
                                    /*createOpTime=*/repl::OpTime{Timestamp{1, 1}, 1},
                                    /*createCollCatalogIdentifier=*/boost::none,
                                    /*fromMigrate=*/false,
                                    /*isTimeseries=*/false,
                                    /*recordIdsReplicated=*/false);
        wuow.commit();
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, uuid, /*expectPersisted=*/true, /*expectedCount=*/0, /*expectedSize=*/0);
}

TEST_F(ReplicatedFastCountOpObserverTest, DoNotAddEntryIfCreationAborted) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const UUID uuid = UUID::gen();
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "newColl");

    CollectionOptions options;
    options.uuid = uuid;

    replicated_fast_count::ReplicatedFastCountOpObserver observer;

    {
        WriteUnitOfWork wuow(_opCtx);
        observer.onCreateCollection(_opCtx,
                                    nss,
                                    options,
                                    /*idIndex=*/BSONObj{},
                                    /*createOpTime=*/repl::OpTime{Timestamp{1, 1}, 1},
                                    /*createCollCatalogIdentifier=*/boost::none,
                                    /*fromMigrate=*/false,
                                    /*isTimeseries=*/false,
                                    /*recordIdsReplicated=*/false);
        // Destruct wuow before committing, which should abort the collection creation.
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, uuid, /*expectPersisted=*/false, /*expectedCount=*/0, /*expectedSize=*/0);
}

TEST_F(ReplicatedFastCountOpObserverTest, CollectionDropRemovesEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    NamespaceString nssToDrop = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "collectionToDrop");

    ASSERT_OK(createCollection(_opCtx, nssToDrop.dbName(), BSON("create" << nssToDrop.coll())));

    UUID dropCollUUID = UUID::gen();
    {
        AutoGetCollection coll(_opCtx, nssToDrop, LockMode::MODE_IS);
        dropCollUUID = coll->uuid();
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, dropCollUUID, /*expectPersisted=*/true, /*expectedCount=*/0, /*expectedSize=*/0);

    {
        DropReply reply;
        ASSERT_OK(dropCollection(_opCtx,
                                 nssToDrop,
                                 &reply,
                                 DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops,
                                 false /* fromMigrate */));
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, dropCollUUID, /*expectPersisted=*/false, /*expectedCount=*/0, /*expectedSize=*/0);
}

TEST_F(ReplicatedFastCountOpObserverTest, DoNotRemoveEntryIfDropAborted) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "newlyCreatedCollection");
    const UUID newCollUUID = UUID::gen();

    CollectionOptions options;
    options.uuid = newCollUUID;

    replicated_fast_count::ReplicatedFastCountOpObserver observer;

    {
        WriteUnitOfWork wuow(_opCtx);
        observer.onCreateCollection(_opCtx,
                                    newNss,
                                    options,
                                    /*idIndex=*/BSONObj{},
                                    /*createOpTime=*/repl::OpTime{Timestamp{1, 1}, 1},
                                    /*createCollCatalogIdentifier=*/boost::none,
                                    /*fromMigrate=*/false,
                                    /*isTimeseries=*/false,
                                    /*recordIdsReplicated=*/false);
        wuow.commit();
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, newCollUUID, /*expectPersisted=*/true, /*expectedCount=*/0, /*expectedSize=*/0);

    {
        WriteUnitOfWork wuow(_opCtx);
        observer.onDropCollection(_opCtx,
                                  newNss,
                                  newCollUUID,
                                  /*numRecords=*/0,
                                  /*markFromMigrate=*/false,
                                  /*isTimeseries=*/false);
        // Destruct wuow before it commits, which should abort the collection drop.
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, newCollUUID, /*expectPersisted=*/true, /*expectedCount=*/0, /*expectedSize=*/0);
}

TEST_F(ReplicatedFastCountOpObserverTest, InitialEntryTimestampMatchesPrimaryAndSecondary) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    replicated_fast_count::ReplicatedFastCountOpObserver observer;

    const UUID primaryUUID = UUID::gen();
    {
        CollectionOptions options;
        options.uuid = primaryUUID;
        WriteUnitOfWork wuow(_opCtx);
        observer.onCreateCollection(_opCtx,
                                    NamespaceString::createNamespaceString_forTest(
                                        "replicated_fast_count_test", "primaryColl"),
                                    options,
                                    /*idIndex=*/BSONObj{},
                                    /*createOpTime=*/repl::OpTime{},
                                    /*createCollCatalogIdentifier=*/boost::none,
                                    /*fromMigrate=*/false,
                                    /*isTimeseries=*/false,
                                    /*recordIdsReplicated=*/false);
        wuow.commit();
    }

    const UUID secondaryUUID = UUID::gen();
    {
        CollectionOptions options;
        options.uuid = secondaryUUID;
        repl::UnreplicatedWritesBlock uwb(_opCtx);
        WriteUnitOfWork wuow(_opCtx);
        observer.onCreateCollection(_opCtx,
                                    NamespaceString::createNamespaceString_forTest(
                                        "replicated_fast_count_test", "secondaryColl"),
                                    options,
                                    /*idIndex=*/BSONObj{},
                                    /*createOpTime=*/repl::OpTime{Timestamp{10, 1}, 1},
                                    /*createCollCatalogIdentifier=*/boost::none,
                                    /*fromMigrate=*/false,
                                    /*isTimeseries=*/false,
                                    /*recordIdsReplicated=*/false);
        wuow.commit();
    }

    auto primaryEntry = replicated_fast_count::SizeCountStore{}.read(_opCtx, primaryUUID);
    auto secondaryEntry = replicated_fast_count::SizeCountStore{}.read(_opCtx, secondaryUUID);

    ASSERT_TRUE(primaryEntry.has_value());
    ASSERT_TRUE(secondaryEntry.has_value());
    EXPECT_EQ(primaryEntry->timestamp, secondaryEntry->timestamp);
}

/**
 * Calling onDropCollection for a UUID that has no fast count entry throws an assertion error.
 */
TEST_F(ReplicatedFastCountOpObserverTest, DropCollectionWithoutEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const UUID uuid = UUID::gen();
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "neverCreated");

    replicated_fast_count::ReplicatedFastCountOpObserver observer;

    // There is no fast count entry for this UUID, so the drop asserts.
    ASSERT_THROWS_CODE(
        [&] {
            WriteUnitOfWork wuow(_opCtx);
            observer.onDropCollection(_opCtx,
                                      nss,
                                      uuid,
                                      /*numRecords=*/0,
                                      /*markFromMigrate=*/false,
                                      /*isTimeseries=*/false);
            wuow.commit();
        }(),
        DBException,
        12054101);
}

}  // namespace
}  // namespace mongo
