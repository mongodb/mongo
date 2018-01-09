/**
 *    Copyright 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;

class CreateCollectionTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    // Use StorageInterface to access storage features below catalog interface.
    std::unique_ptr<repl::StorageInterface> _storage;
};

void CreateCollectionTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    _storage = stdx::make_unique<repl::StorageInterfaceImpl>();
}

void CreateCollectionTest::tearDown() {
    _storage = {};

    // Tear down mongod.
    ServiceContextMongoDTest::tearDown();
}

/**
 * Creates an OperationContext.
 */
ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

/**
 * Returns true if collection exists.
 */
bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return AutoGetCollectionForRead(opCtx, nss).getCollection() != nullptr;
}

/**
 * Returns collection options.
 */
CollectionOptions getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead autoColl(opCtx, nss);
    auto collection = autoColl.getCollection();
    ASSERT_TRUE(collection) << "Unable to get collections options for " << nss
                            << " because collection does not exist.";
    auto catalogEntry = collection->getCatalogEntry();
    return catalogEntry->getCollectionOptions(opCtx);
}

/**
 * Returns UUID of collection.
 */
CollectionUUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    auto options = getCollectionOptions(opCtx, nss);
    ASSERT_TRUE(options.uuid);
    return *(options.uuid);
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsWithSpecificUuidNoExistingCollection) {
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.db(), MODE_X);
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.db().toString(),
                                          uuid.toBSON()["uuid"],
                                          BSON("create" << newNss.coll())));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidNonDropPendingCurrentCollectionHasSameUuid) {
    NamespaceString curNss("test.curColl");
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.db(), MODE_X);

    // Create existing collection using StorageInterface.
    {
        CollectionOptions options;
        options.uuid = uuid;
        ASSERT_OK(_storage->createCollection(opCtx.get(), curNss, options));
    }
    ASSERT_TRUE(collectionExists(opCtx.get(), curNss));
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    // This should rename the existing collection 'curNss' to the collection 'newNss' we are trying
    // to create.
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.db().toString(),
                                          uuid.toBSON()["uuid"],
                                          BSON("create" << newNss.coll())));

    ASSERT_FALSE(collectionExists(opCtx.get(), curNss));
    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidRenamesExistingCollectionWithSameNameOutOfWay) {
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.db(), MODE_X);

    // Create existing collection with same name but different UUID using StorageInterface.
    auto existingCollectionUuid = UUID::gen();
    {
        CollectionOptions options;
        options.uuid = existingCollectionUuid;
        ASSERT_OK(_storage->createCollection(opCtx.get(), newNss, options));
    }
    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
    ASSERT_NOT_EQUALS(uuid, getCollectionUuid(opCtx.get(), newNss));

    // This should rename the existing collection 'newNss' to a randomly generated collection name.
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.db().toString(),
                                          uuid.toBSON()["uuid"],
                                          BSON("create" << newNss.coll())));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
    ASSERT_EQUALS(uuid, getCollectionUuid(opCtx.get(), newNss));

    // Check that old collection that was renamed out of the way still exists.
    auto& uuidCatalog = UUIDCatalog::get(opCtx.get());
    auto renamedCollectionNss = uuidCatalog.lookupNSSByUUID(existingCollectionUuid);
    ASSERT_TRUE(collectionExists(opCtx.get(), renamedCollectionNss))
        << "old renamed collection with UUID " << existingCollectionUuid
        << " missing: " << renamedCollectionNss;
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidReturnsNamespaceExistsIfCollectionIsDropPending) {
    NamespaceString curNss("test.curColl");
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = curNss.makeDropPendingNamespace(dropOpTime);
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.db(), MODE_X);

    // Create drop pending collection using StorageInterface.
    {
        CollectionOptions options;
        options.uuid = uuid;
        ASSERT_OK(_storage->createCollection(opCtx.get(), dropPendingNss, options));
    }
    ASSERT_TRUE(collectionExists(opCtx.get(), dropPendingNss));
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    // This should fail because we are not allowed to take a collection out of its drop-pending
    // state.
    ASSERT_EQUALS(ErrorCodes::NamespaceExists,
                  createCollectionForApplyOps(opCtx.get(),
                                              newNss.db().toString(),
                                              uuid.toBSON()["uuid"],
                                              BSON("create" << newNss.coll())));

    ASSERT_TRUE(collectionExists(opCtx.get(), dropPendingNss));
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));
}

}  // namespace
