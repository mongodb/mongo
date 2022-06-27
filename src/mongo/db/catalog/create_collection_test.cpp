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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;

class CreateCollectionTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    void validateValidator(const std::string& validatorStr, int expectedError);

    // Use StorageInterface to access storage features below catalog interface.
    std::unique_ptr<repl::StorageInterface> _storage;
};

void CreateCollectionTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    _storage = std::make_unique<repl::StorageInterfaceImpl>();
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
    auto opCtx = cc().makeOperationContext();
    repl::createOplog(opCtx.get());
    return opCtx;
}

void CreateCollectionTest::validateValidator(const std::string& validatorStr,
                                             const int expectedError) {
    NamespaceString newNss("test.newCollWithValidation");

    auto opCtx = makeOpCtx();

    CollectionOptions options;
    options.validator = fromjson(validatorStr);
    options.uuid = UUID::gen();

    return writeConflictRetry(opCtx.get(), "create", newNss.ns(), [&] {
        AutoGetCollection autoColl(opCtx.get(), newNss, MODE_IX);
        auto db = autoColl.ensureDbExists(opCtx.get());
        ASSERT_TRUE(db) << "Cannot create collection " << newNss << " because database "
                        << newNss.db() << " does not exist.";

        WriteUnitOfWork wuow(opCtx.get());
        const auto status =
            db->userCreateNS(opCtx.get(), newNss, options, false /*createDefaultIndexes*/);
        ASSERT_EQ(expectedError, status.code()) << status;
    });
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
    AutoGetCollectionForRead collection(opCtx, nss);
    ASSERT_TRUE(collection) << "Unable to get collections options for " << nss
                            << " because collection does not exist.";
    return collection->getCollectionOptions();
}

/**
 * Returns UUID of collection.
 */
UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    auto options = getCollectionOptions(opCtx, nss);
    ASSERT_TRUE(options.uuid);
    return *(options.uuid);
}

TEST_F(CreateCollectionTest, CreateCollectionForApplyOpsWithSpecificUuidNoExistingCollection) {
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));

    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.dbName(), MODE_IX);
    ASSERT_OK(createCollectionForApplyOps(opCtx.get(),
                                          newNss.db().toString(),
                                          uuid,
                                          BSON("create" << newNss.coll()),
                                          /*allowRenameOutOfTheWay*/ false));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidNonDropPendingCurrentCollectionHasSameUuid) {
    NamespaceString curNss("test.curColl");
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.

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
                                          uuid,
                                          BSON("create" << newNss.coll()),
                                          /*allowRenameOutOfTheWay*/ true));

    ASSERT_FALSE(collectionExists(opCtx.get(), curNss));
    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidRenamesExistingCollectionWithSameNameOutOfWay) {
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.

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
                                          uuid,
                                          BSON("create" << newNss.coll()),
                                          /*allowRenameOutOfTheWay*/ true));

    ASSERT_TRUE(collectionExists(opCtx.get(), newNss));
    ASSERT_EQUALS(uuid, getCollectionUuid(opCtx.get(), newNss));

    // Check that old collection that was renamed out of the way still exists.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto renamedCollectionNss = catalog->lookupNSSByUUID(opCtx.get(), existingCollectionUuid);
    ASSERT(renamedCollectionNss);
    ASSERT_TRUE(collectionExists(opCtx.get(), *renamedCollectionNss))
        << "old renamed collection with UUID " << existingCollectionUuid
        << " missing: " << *renamedCollectionNss;
}

TEST_F(CreateCollectionTest,
       CreateCollectionForApplyOpsWithSpecificUuidReturnsNamespaceExistsIfCollectionIsDropPending) {
    NamespaceString curNss("test.curColl");
    repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
    auto dropPendingNss = curNss.makeDropPendingNamespace(dropOpTime);
    NamespaceString newNss("test.newColl");

    auto opCtx = makeOpCtx();
    auto uuid = UUID::gen();
    Lock::DBLock lock(opCtx.get(), newNss.dbName(), MODE_IX);

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
                                              uuid,
                                              BSON("create" << newNss.coll()),
                                              /*allowRenameOutOfTheWay*/ false));

    ASSERT_TRUE(collectionExists(opCtx.get(), dropPendingNss));
    ASSERT_FALSE(collectionExists(opCtx.get(), newNss));
}

TEST_F(CreateCollectionTest, ValidationOptions) {
    // Try a valid validator before trying invalid validators.
    validateValidator("", static_cast<int>(ErrorCodes::Error::OK));
    validateValidator("{a: {$exists: false}}", static_cast<int>(ErrorCodes::Error::OK));

    // Invalid validators.
    validateValidator(
        "{$expr: {$function: {body: 'function(age) { return age >= 21; }', args: ['$age'], lang: "
        "'js'}}}",
        4660800);
    validateValidator("{$expr: {$_internalJsEmit: {eval: 'function() {}', this: {}}}}", 4660801);
    validateValidator("{$where: 'this.a == this.b'}", static_cast<int>(ErrorCodes::BadValue));
}

// Tests that validator validation is disabled when inserting a document into a
// <database>.system.resharding.* collection. The primary donor is responsible for validating
// documents before they are inserted into the recipient's temporary resharding collection.
TEST_F(CreateCollectionTest, ValidationDisabledForTemporaryReshardingCollection) {
    NamespaceString reshardingNss("myDb", "system.resharding.yay");
    auto opCtx = makeOpCtx();

    Lock::GlobalLock lk(opCtx.get(), MODE_X);  // Satisfy low-level locking invariants.
    BSONObj createCmdObj = BSON("create" << reshardingNss.coll() << "validator" << BSON("a" << 5));
    ASSERT_OK(createCollection(opCtx.get(), reshardingNss.db().toString(), createCmdObj));
    ASSERT_TRUE(collectionExists(opCtx.get(), reshardingNss));

    AutoGetCollection collection(opCtx.get(), reshardingNss, MODE_X);

    WriteUnitOfWork wuow(opCtx.get());
    // Ensure a document that violates validator criteria can be inserted into the temporary
    // resharding collection.
    auto insertObj = fromjson("{'_id':2, a:1}");
    auto status =
        collection->insertDocument(opCtx.get(), InsertStatement(insertObj), nullptr, false);
    ASSERT_OK(status);
}

}  // namespace
