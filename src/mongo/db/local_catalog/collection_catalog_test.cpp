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

#include "mongo/db/local_catalog/collection_catalog.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds/index_build_block.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_yield_restore.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/resource_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/uncommitted_catalog_updates.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <map>

namespace mongo {
namespace {

/**
 * A test fixture that creates a CollectionCatalog and const CollectionPtr& pointer to store in it.
 */
class CollectionCatalogTest : public ServiceContextMongoDTest {
public:
    CollectionCatalogTest()
        : nss(NamespaceString::createNamespaceString_forTest("testdb", "testcol")),
          col(CollectionPtr::CollectionPtr_UNSAFE(nullptr)),
          colUUID(UUID::gen()),
          nextUUID(UUID::gen()),
          prevUUID(UUID::gen()) {
        if (prevUUID > colUUID)
            std::swap(prevUUID, colUUID);
        if (colUUID > nextUUID)
            std::swap(colUUID, nextUUID);
        if (prevUUID > colUUID)
            std::swap(prevUUID, colUUID);
        ASSERT_GT(colUUID, prevUUID);
        ASSERT_GT(nextUUID, colUUID);
    }

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
        globalLock.emplace(opCtx.get());

        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(colUUID, nss);
        // The lifetime of the collection returned by the lookup is guaranteed to be valid as
        // it's controlled by the test. The initialization is therefore safe.
        col = CollectionPtr::CollectionPtr_UNSAFE(collection.get());
        // Register dummy collection in catalog.
        catalog.registerCollection(opCtx.get(), collection, boost::none);

        repl::ReplicationCoordinator::set(
            getServiceContext(),
            std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext()));
    }

    void tearDown() override {
        globalLock.reset();
    }

protected:
    std::shared_ptr<CollectionCatalog> sharedCatalog = std::make_shared<CollectionCatalog>();
    CollectionCatalog& catalog = *sharedCatalog;
    ServiceContext::UniqueOperationContext opCtx;
    boost::optional<Lock::GlobalWrite> globalLock;
    NamespaceString nss;
    CollectionPtr col;
    UUID colUUID;
    UUID nextUUID;
    UUID prevUUID;
};

class CollectionCatalogIterationTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
        globalLock.emplace(opCtx.get());

        for (int counter = 0; counter < 5; ++counter) {
            NamespaceString fooNss = NamespaceString::createNamespaceString_forTest(
                "foo", "coll" + std::to_string(counter));
            NamespaceString barNss = NamespaceString::createNamespaceString_forTest(
                "bar", "coll" + std::to_string(counter));

            std::shared_ptr<Collection> fooColl = std::make_shared<CollectionMock>(fooNss);
            std::shared_ptr<Collection> barColl = std::make_shared<CollectionMock>(barNss);

            // The lifetime of the collection returned by the lookup is guaranteed to be valid as
            // it's controlled by the test. The initialization is therefore safe.
            dbMap["foo"].insert(std::make_pair(fooColl->uuid(),
                                               CollectionPtr::CollectionPtr_UNSAFE(fooColl.get())));
            dbMap["bar"].insert(std::make_pair(barColl->uuid(),
                                               CollectionPtr::CollectionPtr_UNSAFE(barColl.get())));

            catalog.registerCollection(opCtx.get(), fooColl, boost::none);
            catalog.registerCollection(opCtx.get(), barColl, boost::none);
        }
    }

    void tearDown() override {
        for (auto& it : dbMap) {
            for (auto& kv : it.second) {
                catalog.deregisterCollection(opCtx.get(), kv.first, boost::none);
            }
        }
        globalLock.reset();
    }

    std::map<UUID, CollectionPtr>::iterator collsIterator(std::string dbName) {
        auto it = dbMap.find(dbName);
        ASSERT(it != dbMap.end());
        return it->second.begin();
    }

    std::map<UUID, CollectionPtr>::iterator collsIteratorEnd(std::string dbName) {
        auto it = dbMap.find(dbName);
        ASSERT(it != dbMap.end());
        return it->second.end();
    }

    void checkCollections(const DatabaseName& dbName) {
        unsigned long counter = 0;
        const auto dbNameStr = dbName.toString_forTest();

        auto orderedIt = collsIterator(dbNameStr);
        auto catalogRange = catalog.range(dbName);
        auto catalogIt = catalogRange.begin();
        for (; catalogIt != catalogRange.end() && orderedIt != collsIteratorEnd(dbNameStr);
             ++catalogIt, ++orderedIt) {

            auto catalogColl = *catalogIt;
            ASSERT(catalogColl);
            const auto& orderedColl = orderedIt->second;
            ASSERT_EQ(catalogColl->ns(), orderedColl->ns());
            ++counter;
        }

        ASSERT_EQUALS(counter, dbMap[dbNameStr].size());
    }

    void dropColl(const std::string dbName, UUID uuid) {
        dbMap[dbName].erase(uuid);
    }

protected:
    CollectionCatalog catalog;
    ServiceContext::UniqueOperationContext opCtx;
    boost::optional<Lock::GlobalWrite> globalLock;
    std::map<std::string, std::map<UUID, CollectionPtr>> dbMap;
};

class CollectionCatalogResourceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
        globalLock.emplace(opCtx.get());

        for (int i = 0; i < 5; i++) {
            NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                "resourceDb", "coll" + std::to_string(i));
            std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);

            catalog.registerCollection(opCtx.get(), std::move(collection), boost::none);
        }

        int numEntries = 0;
        for (auto&& coll :
             catalog.range(DatabaseName::createDatabaseName_forTest(boost::none, "resourceDb"))) {
            auto collName = coll->ns();
            ResourceId rid(RESOURCE_COLLECTION, collName);

            ASSERT_NE(ResourceCatalog::get().name(rid), boost::none);
            numEntries++;
        }
        ASSERT_EQ(5, numEntries);
    }

    void tearDown() override {
        std::vector<UUID> collectionsToDeregister;
        for (auto&& coll :
             catalog.range(DatabaseName::createDatabaseName_forTest(boost::none, "resourceDb"))) {
            auto uuid = coll->uuid();
            if (!coll) {
                break;
            }

            collectionsToDeregister.push_back(uuid);
        }

        for (auto&& uuid : collectionsToDeregister) {
            catalog.deregisterCollection(opCtx.get(), uuid, boost::none);
        }

        int numEntries = 0;
        for ([[maybe_unused]] auto&& coll :
             catalog.range(DatabaseName::createDatabaseName_forTest(boost::none, "resourceDb"))) {
            numEntries++;
        }
        ASSERT_EQ(0, numEntries);
        globalLock.reset();
    }

protected:
    ServiceContext::UniqueOperationContext opCtx;
    CollectionCatalog catalog;
    boost::optional<Lock::GlobalWrite> globalLock;
};

TEST_F(CollectionCatalogResourceTest, RemoveAllResources) {
    catalog.deregisterAllCollectionsAndViews(getServiceContext());

    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "resourceDb");
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    ASSERT_EQ(boost::none, ResourceCatalog::get().name(rid));

    for (int i = 0; i < 5; i++) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            "resourceDb", "coll" + std::to_string(i));
        rid = ResourceId(RESOURCE_COLLECTION, nss);
        ASSERT_EQ(boost::none, ResourceCatalog::get().name(rid));
    }
}

TEST_F(CollectionCatalogResourceTest, LookupDatabaseResource) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "resourceDb");
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    auto ridStr = ResourceCatalog::get().name(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(dbName.toStringWithTenantId_forTest()) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingDatabaseResource) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "missingDb");
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    ASSERT(!ResourceCatalog::get().name(rid));
}

TEST_F(CollectionCatalogResourceTest, LookupCollectionResource) {
    const NamespaceString collNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "resourceDb.coll1");
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    auto ridStr = ResourceCatalog::get().name(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(collNs.toStringWithTenantId_forTest()) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingCollectionResource) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "resourceDb.coll5");
    auto rid = ResourceId(RESOURCE_COLLECTION, nss);
    ASSERT(!ResourceCatalog::get().name(rid));
}

TEST_F(CollectionCatalogResourceTest, RemoveCollection) {
    const NamespaceString collNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "resourceDb.coll1");
    auto coll = catalog.lookupCollectionByNamespace(opCtx.get(), NamespaceString(collNs));
    catalog.deregisterCollection(opCtx.get(), coll->uuid(), boost::none);
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    ASSERT(!ResourceCatalog::get().name(rid));
}

// Create an iterator over the CollectionCatalog and assert that all collections are present.
// Iteration ends when the end of the catalog is reached.
TEST_F(CollectionCatalogIterationTest, EndAtEndOfCatalog) {
    checkCollections(DatabaseName::createDatabaseName_forTest(boost::none, "foo"));
}

// Create an iterator over the CollectionCatalog and test that all collections are present.
// Iteration ends
// when the end of a database-specific section of the catalog is reached.
TEST_F(CollectionCatalogIterationTest, EndAtEndOfSection) {
    checkCollections(DatabaseName::createDatabaseName_forTest(boost::none, "bar"));
}

TEST_F(CollectionCatalogIterationTest, GetUUIDWontRepositionEvenIfEntryIsDropped) {
    auto range = catalog.range(DatabaseName::createDatabaseName_forTest(boost::none, "bar"));
    auto it = range.begin();
    auto collsIt = collsIterator("bar");
    auto uuid = collsIt->first;
    catalog.deregisterCollection(opCtx.get(), uuid, boost::none);
    dropColl("bar", uuid);

    ASSERT_EQUALS(uuid, (*it)->uuid());
}

TEST_F(CollectionCatalogTest, OnCreateCollection) {
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == col.get());
}

TEST_F(CollectionCatalogTest, LookupCollectionByUUID) {
    // Ensure the string value of the NamespaceString of the obtained Collection is equal to
    // nss.ns_forTest().
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), colUUID)->ns().ns_forTest(),
                  nss.ns_forTest());
    // Ensure lookups of unknown UUIDs result in null pointers.
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), UUID::gen()) == nullptr);
}

TEST_F(CollectionCatalogTest, LookupNSSByUUID) {
    // Ensure the string value of the obtained NamespaceString is equal to nss.ns_forTest().
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), colUUID)->ns_forTest(), nss.ns_forTest());
    // Ensure namespace lookups of unknown UUIDs result in empty NamespaceStrings.
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), UUID::gen()), boost::none);
}

TEST_F(CollectionCatalogTest, InsertAfterLookup) {
    auto newUUID = UUID::gen();
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest(nss.dbName(), "newcol");
    std::shared_ptr<Collection> newCollShared = std::make_shared<CollectionMock>(newUUID, newNss);
    auto newCol = newCollShared.get();

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), newUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), newUUID), boost::none);
    catalog.registerCollection(opCtx.get(), std::move(newCollShared), boost::none);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), newUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);
}

TEST_F(CollectionCatalogTest, OnDropCollection) {
    // The lifetime of the collection returned by the lookup is guaranteed to be valid as
    // it's controlled by the test. The initialization is therefore safe.
    CollectionPtr yieldableColl =
        CollectionPtr::CollectionPtr_UNSAFE(catalog.lookupCollectionByUUID(opCtx.get(), colUUID));
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, col);

    // Make the CollectionPtr yieldable by setting yield impl
    yieldableColl.makeYieldable(opCtx.get(),
                                LockedCollectionYieldRestore(opCtx.get(), yieldableColl));

    // Yielding resets a CollectionPtr's internal state to be restored later, provided
    // the collection has not been dropped or renamed.
    ASSERT_EQ(yieldableColl->uuid(), colUUID);  // Correct collection UUID is required for restore.
    yieldableColl.yield();
    ASSERT_FALSE(yieldableColl);

    // The global catalog is used to refresh the CollectionPtr's internal state, so we temporarily
    // replace the global instance initialized in the service context test fixture with our own.
    CollectionCatalog::stash(opCtx.get(), sharedCatalog);

    // Before dropping collection, confirm that the CollectionPtr can be restored successfully.
    yieldableColl.restore();
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, col);

    // Reset CollectionPtr for post-drop restore test.
    yieldableColl.yield();
    ASSERT_FALSE(yieldableColl);

    catalog.deregisterCollection(opCtx.get(), colUUID, boost::none);
    // Ensure the lookup returns a null pointer upon removing the colUUID entry.
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);

    // After dropping the collection, we should fail to restore the CollectionPtr.
    yieldableColl.restore();
    ASSERT_FALSE(yieldableColl);
}

TEST_F(CollectionCatalogTest, RenameCollection) {
    auto uuid = UUID::gen();
    NamespaceString oldNss = NamespaceString::createNamespaceString_forTest(nss.dbName(), "oldcol");
    std::shared_ptr<Collection> collShared = std::make_shared<CollectionMock>(uuid, oldNss);
    auto collection = collShared.get();
    catalog.registerCollection(opCtx.get(), std::move(collShared), boost::none);
    // The lifetime of the collection returned by the lookup is guaranteed to be valid as
    // it's controlled by the test. The initialization is therefore safe.
    CollectionPtr yieldableColl =
        CollectionPtr::CollectionPtr_UNSAFE(catalog.lookupCollectionByUUID(opCtx.get(), uuid));
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, CollectionPtr::CollectionPtr_UNSAFE(collection));

    // Make the CollectionPtr yieldable by setting yield impl
    yieldableColl.makeYieldable(opCtx.get(),
                                LockedCollectionYieldRestore(opCtx.get(), yieldableColl));

    // Yielding resets a CollectionPtr's internal state to be restored later, provided
    // the collection has not been dropped or renamed.
    ASSERT_EQ(yieldableColl->uuid(), uuid);  // Correct collection UUID is required for restore.
    yieldableColl.yield();
    ASSERT_FALSE(yieldableColl);

    // The global catalog is used to refresh the CollectionPtr's internal state, so we temporarily
    // replace the global instance initialized in the service context test fixture with our own.
    CollectionCatalog::stash(opCtx.get(), sharedCatalog);

    // Before renaming collection, confirm that the CollectionPtr can be restored successfully.
    yieldableColl.restore();
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, CollectionPtr::CollectionPtr_UNSAFE(collection));

    // Reset CollectionPtr for post-rename restore test.
    yieldableColl.yield();
    ASSERT_FALSE(yieldableColl);

    NamespaceString newNss = NamespaceString::createNamespaceString_forTest(nss.dbName(), "newcol");
    ASSERT_OK(collection->rename(opCtx.get(), newNss, false));
    ASSERT_EQ(collection->ns(), newNss);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), uuid), collection);

    // After renaming the collection, we should fail to restore the CollectionPtr.
    yieldableColl.restore();
    ASSERT_FALSE(yieldableColl);
}

TEST_F(CollectionCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsOldNSSIfDropped) {
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog();
    }

    catalog.deregisterCollection(opCtx.get(), colUUID, boost::none);
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);

    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onOpenCatalog();
    }

    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), colUUID), boost::none);
}

TEST_F(CollectionCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsNewlyCreatedNSS) {
    auto newUUID = UUID::gen();
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest(nss.dbName(), "newcol");
    std::shared_ptr<Collection> newCollShared = std::make_shared<CollectionMock>(newUUID, newNss);
    auto newCol = newCollShared.get();

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog();
    }

    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), newUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), newUUID), boost::none);
    catalog.registerCollection(opCtx.get(), std::move(newCollShared), boost::none);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), newUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);

    // Ensure that collection still exists after opening the catalog again.
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onOpenCatalog();
    }

    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), newUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);
}

TEST_F(CollectionCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsFreshestNSS) {
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest(nss.dbName(), "newcol");
    std::shared_ptr<Collection> newCollShared = std::make_shared<CollectionMock>(colUUID, newNss);
    auto newCol = newCollShared.get();

    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog();
    }

    catalog.deregisterCollection(opCtx.get(), colUUID, boost::none);
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);
    {
        Lock::GlobalWrite lk(opCtx.get());
        catalog.registerCollection(opCtx.get(), std::move(newCollShared), boost::none);
    }

    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), colUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), newNss);

    // Ensure that collection still exists after opening the catalog again.
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onOpenCatalog();
    }

    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), colUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), newNss);
}

// Re-opening the catalog should increment the CollectionCatalog's epoch.
TEST_F(CollectionCatalogTest, CollectionCatalogEpoch) {
    auto originalEpoch = catalog.getEpoch();

    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog();
        catalog.onOpenCatalog();
    }

    auto incrementedEpoch = catalog.getEpoch();
    ASSERT_EQ(originalEpoch + 1, incrementedEpoch);
}

TEST_F(CollectionCatalogTest, GetAllCollectionNamesAndGetAllDbNames) {
    NamespaceString aColl = NamespaceString::createNamespaceString_forTest("dbA", "collA");
    NamespaceString b1Coll = NamespaceString::createNamespaceString_forTest("dbB", "collB1");
    NamespaceString b2Coll = NamespaceString::createNamespaceString_forTest("dbB", "collB2");
    NamespaceString cColl = NamespaceString::createNamespaceString_forTest("dbC", "collC");
    NamespaceString d1Coll = NamespaceString::createNamespaceString_forTest("dbD", "collD1");
    NamespaceString d2Coll = NamespaceString::createNamespaceString_forTest("dbD", "collD2");
    NamespaceString d3Coll = NamespaceString::createNamespaceString_forTest("dbD", "collD3");

    std::vector<NamespaceString> nsss = {aColl, b1Coll, b2Coll, cColl, d1Coll, d2Coll, d3Coll};
    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        catalog.registerCollection(opCtx.get(), std::move(newColl), boost::none);
    }

    std::vector<NamespaceString> dCollList = {d1Coll, d2Coll, d3Coll};

    Lock::DBLock dbLock(opCtx.get(), d1Coll.dbName(), MODE_S);
    auto res = catalog.getAllCollectionNamesFromDb(opCtx.get(), d1Coll.dbName());
    std::sort(res.begin(), res.end());
    ASSERT(res == dCollList);

    std::vector<DatabaseName> dbNames = {
        DatabaseName::createDatabaseName_forTest(boost::none, "dbA"),
        DatabaseName::createDatabaseName_forTest(boost::none, "dbB"),
        DatabaseName::createDatabaseName_forTest(boost::none, "dbC"),
        DatabaseName::createDatabaseName_forTest(boost::none, "dbD"),
        DatabaseName::createDatabaseName_forTest(boost::none, "testdb")};
    ASSERT(catalog.getAllDbNames() == dbNames);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

TEST_F(CollectionCatalogTest, GetAllDbNamesForTenantMultitenancyFalse) {
    TenantId tid1 = TenantId(OID::gen());
    TenantId tid2 = TenantId(OID::gen());
    // This is extremely contrived as we shouldn't be able to create nss's with tenantIds in
    // multitenancySupport=false mode, but the behavior of getAllDbNamesForTenant should be well
    // defined even in the event of a rollback.
    NamespaceString testDb = NamespaceString::createNamespaceString_forTest(boost::none, "testdb");
    NamespaceString dbA = NamespaceString::createNamespaceString_forTest(tid1, "dbA.collA");
    NamespaceString dbB = NamespaceString::createNamespaceString_forTest(tid1, "dbB.collA");
    NamespaceString dbC = NamespaceString::createNamespaceString_forTest(tid1, "dbC.collA");
    NamespaceString dbD = NamespaceString::createNamespaceString_forTest(tid2, "dbB.collA");

    std::vector<NamespaceString> nsss = {testDb, dbA, dbB, dbC, dbD};
    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        catalog.registerCollection(opCtx.get(), std::move(newColl), boost::none);
    }

    std::vector<DatabaseName> allDbNames = {
        DatabaseName::createDatabaseName_forTest(boost::none, "testdb"),
        DatabaseName::createDatabaseName_forTest(tid1, "dbA"),
        DatabaseName::createDatabaseName_forTest(tid1, "dbB"),
        DatabaseName::createDatabaseName_forTest(tid1, "dbC"),
        DatabaseName::createDatabaseName_forTest(tid2, "dbB")};
    ASSERT_EQ(catalog.getAllDbNamesForTenant(boost::none), allDbNames);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

TEST_F(CollectionCatalogTest, GetAllDbNamesForTenant) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    TenantId tid1 = TenantId(OID::gen());
    TenantId tid2 = TenantId(OID::gen());
    NamespaceString dbA = NamespaceString::createNamespaceString_forTest(tid1, "dbA.collA");
    NamespaceString dbB = NamespaceString::createNamespaceString_forTest(tid1, "dbB.collA");
    NamespaceString dbC = NamespaceString::createNamespaceString_forTest(tid1, "dbC.collA");
    NamespaceString dbD = NamespaceString::createNamespaceString_forTest(tid2, "dbB.collA");

    std::vector<NamespaceString> nsss = {dbA, dbB, dbC, dbD};
    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        catalog.registerCollection(opCtx.get(), std::move(newColl), boost::none);
    }

    std::vector<DatabaseName> dbNamesForTid1 = {
        DatabaseName::createDatabaseName_forTest(tid1, "dbA"),
        DatabaseName::createDatabaseName_forTest(tid1, "dbB"),
        DatabaseName::createDatabaseName_forTest(tid1, "dbC")};
    ASSERT_EQ(catalog.getAllDbNamesForTenant(tid1), dbNamesForTid1);

    std::vector<DatabaseName> dbNamesForTid2 = {
        DatabaseName::createDatabaseName_forTest(tid2, "dbB")};
    ASSERT_EQ(catalog.getAllDbNamesForTenant(tid2), dbNamesForTid2);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

TEST_F(CollectionCatalogTest, GetAllTenantsMultitenancyFalse) {
    std::vector<NamespaceString> nsss = {
        NamespaceString::createNamespaceString_forTest(boost::none, "a"),
        NamespaceString::createNamespaceString_forTest(boost::none, "c"),
        NamespaceString::createNamespaceString_forTest(boost::none, "l")};

    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        catalog.registerCollection(opCtx.get(), std::move(newColl), boost::none);
    }

    ASSERT_EQ(catalog.getAllTenants(), std::set<TenantId>());

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

TEST_F(CollectionCatalogTest, GetAllTenants) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    TenantId tid1 = TenantId(OID::gen());
    TenantId tid2 = TenantId(OID::gen());
    std::vector<NamespaceString> nsss = {
        NamespaceString::createNamespaceString_forTest(boost::none, "a"),
        NamespaceString::createNamespaceString_forTest(boost::none, "c"),
        NamespaceString::createNamespaceString_forTest(boost::none, "l"),
        NamespaceString::createNamespaceString_forTest(tid1, "c"),
        NamespaceString::createNamespaceString_forTest(tid2, "c")};

    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        catalog.registerCollection(opCtx.get(), std::move(newColl), boost::none);
    }

    std::set<TenantId> expectedTenants = {tid1, tid2};
    ASSERT_EQ(catalog.getAllTenants(), expectedTenants);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

TEST_F(CollectionCatalogTest, DropPendingDatabase) {
    auto dbName = DatabaseName::createDatabaseName_forTest(boost::none, "DropPendingDatabase");
    ASSERT_FALSE(catalog.isDropPending(dbName));
    catalog.addDropPending(dbName);
    ASSERT_TRUE(catalog.isDropPending(dbName));
    catalog.addDropPending(dbName);
    ASSERT_TRUE(catalog.isDropPending(dbName));
    catalog.removeDropPending(dbName);
    ASSERT_FALSE(catalog.isDropPending(dbName));
    catalog.removeDropPending(dbName);
    ASSERT_FALSE(catalog.isDropPending(dbName));
}

class ForEachCollectionFromDbTest : public CatalogTestFixture {
public:
    void createTestData() {
        CollectionOptions emptyCollOptions;

        CollectionOptions tempCollOptions;
        tempCollOptions.temp = true;

        ASSERT_OK(storageInterface()->createCollection(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("db", "coll1"),
            emptyCollOptions));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("db", "coll2"),
            tempCollOptions));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("db", "coll3"),
            tempCollOptions));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("db2", "coll4"),
            emptyCollOptions));
    }
};

TEST_F(ForEachCollectionFromDbTest, ForEachCollectionFromDb) {
    createTestData();
    auto opCtx = operationContext();

    {
        const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(opCtx, dbName, MODE_X, [&](const Collection* collection) {
            ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                collection->ns(), MODE_X));
            numCollectionsTraversed++;
            return true;
        });

        ASSERT_EQUALS(numCollectionsTraversed, 3);
    }

    {
        const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db2");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(opCtx, dbName, MODE_IS, [&](const Collection* collection) {
            ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                collection->ns(), MODE_IS));
            numCollectionsTraversed++;
            return true;
        });

        ASSERT_EQUALS(numCollectionsTraversed, 1);
    }

    {
        const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db3");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(opCtx, dbName, MODE_S, [&](const Collection* collection) {
            numCollectionsTraversed++;
            return true;
        });

        ASSERT_EQUALS(numCollectionsTraversed, 0);
    }
}

TEST_F(ForEachCollectionFromDbTest, ForEachCollectionFromDbWithPredicate) {
    createTestData();
    auto opCtx = operationContext();

    {
        const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx,
            dbName,
            MODE_X,
            [&](const Collection* collection) {
                ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                    collection->ns(), MODE_X));
                numCollectionsTraversed++;
                return true;
            },
            [&](const Collection* collection) {
                ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                    collection->ns(), MODE_NONE));
                return collection->getCollectionOptions().temp;
            });

        ASSERT_EQUALS(numCollectionsTraversed, 2);
    }

    {
        const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx,
            dbName,
            MODE_IX,
            [&](const Collection* collection) {
                ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                    collection->ns(), MODE_IX));
                numCollectionsTraversed++;
                return true;
            },
            [&](const Collection* collection) {
                ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                    collection->ns(), MODE_NONE));
                return !collection->getCollectionOptions().temp;
            });

        ASSERT_EQUALS(numCollectionsTraversed, 1);
    }
}

/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts) : _opCtx(opCtx) {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        if (ts.isNull()) {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

class CollectionCatalogTimestampTest : public ServiceContextMongoDTest {
public:
    // Disable table logging. When table logging is enabled, timestamps are discarded by WiredTiger.
    CollectionCatalogTimestampTest()
        : ServiceContextMongoDTest(Options{}.forceDisableTableLogging()) {}

    // Special constructor to _disable_ timestamping. Not to be used directly.
    struct DisableTimestampingTag {};
    CollectionCatalogTimestampTest(DisableTimestampingTag) : ServiceContextMongoDTest() {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
    }

    std::shared_ptr<const CollectionCatalog> catalog() {
        return CollectionCatalog::get(opCtx.get());
    }

    UUID createCollection(OperationContext* opCtx,
                          const NamespaceString& nss,
                          Timestamp timestamp,
                          bool allowMixedModeWrites = false) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        UUID uuid = _createCollection(opCtx, nss, boost::none, allowMixedModeWrites);
        wuow.commit();
        return uuid;
    }

    void createCollectionWithUUID(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  Timestamp timestamp,
                                  UUID uuid) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        _createCollection(opCtx, nss, uuid, false);
        wuow.commit();
    }

    void createCollectionWithUUIDAndLeaveUncommitted(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     Timestamp timestamp,
                                                     UUID uuid,
                                                     boost::optional<WriteUnitOfWork>& wuow) {
        _setupDDLOperation(opCtx, timestamp);
        wuow.emplace(opCtx);
        _createCollection(opCtx, nss, uuid, false);
    }

    void createCollectionWithUUIDAndCommitInNestedUnitOnly(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           Timestamp timestamp,
                                                           UUID uuid,
                                                           boost::optional<WriteUnitOfWork>& wuow) {
        _setupDDLOperation(opCtx, timestamp);
        wuow.emplace(opCtx);
        WriteUnitOfWork nestedWuow(opCtx);
        _createCollection(opCtx, nss, uuid, false);
        nestedWuow.commit();
    }

    void dropCollection(OperationContext* opCtx, const NamespaceString& nss, Timestamp timestamp) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        _dropCollection(opCtx, nss, timestamp);
        wuow.commit();
    }

    void dropCollectionAndLeaveUncommitted(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           Timestamp timestamp,
                                           boost::optional<WriteUnitOfWork>& wuow) {
        _setupDDLOperation(opCtx, timestamp);
        wuow.emplace(opCtx);
        _dropCollection(opCtx, nss, timestamp);
    }

    void renameCollection(OperationContext* opCtx,
                          const NamespaceString& from,
                          const NamespaceString& to,
                          Timestamp timestamp) {
        invariant(from == to);

        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        _renameCollection(opCtx, from, to, timestamp);
        wuow.commit();
    }

    void createIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     BSONObj indexSpec,
                     Timestamp timestamp) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        _createIndex(opCtx, nss, indexSpec);
        wuow.commit();
    }

    void dropIndex(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const std::string& indexName,
                   Timestamp timestamp) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        _dropIndex(opCtx, nss, indexName);
        wuow.commit();
    }

    /**
     * Starts an index build, but leaves the build in progress rather than ready. Returns the
     * IndexBuildBlock performing the build, necessary to finish the build later via
     * finishIndexBuild below.
     */
    std::unique_ptr<IndexBuildBlock> createIndexWithoutFinishingBuild(OperationContext* opCtx,
                                                                      const NamespaceString& nss,
                                                                      BSONObj indexSpec,
                                                                      Timestamp createTimestamp) {
        _setupDDLOperation(opCtx, createTimestamp);

        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        CollectionWriter collection(opCtx, nss);

        auto writableColl = collection.getWritableCollection(opCtx);
        auto storageEngine = getServiceContext()->getStorageEngine();

        // The lifetime of the collection returned by the lookup is guaranteed to be valid as
        // it's controlled by the test. The initialization is therefore safe.
        StatusWith<BSONObj> statusWithSpec = writableColl->getIndexCatalog()->prepareSpecForCreate(
            opCtx, CollectionPtr::CollectionPtr_UNSAFE(writableColl), indexSpec, boost::none);
        uassertStatusOK(statusWithSpec.getStatus());
        indexSpec = statusWithSpec.getValue();

        auto indexBuildBlock = std::make_unique<IndexBuildBlock>(
            writableColl->ns(), indexSpec, IndexBuildMethodEnum::kForeground, UUID::gen());
        IndexBuildInfo indexBuildInfo(
            indexSpec, *storageEngine, nss.dbName(), VersionContext::getDecoration(opCtx));
        uassertStatusOK(indexBuildBlock->init(opCtx,
                                              writableColl,
                                              indexBuildInfo,
                                              /*forRecover=*/false));
        uassertStatusOK(indexBuildBlock->getWritableEntry(opCtx, writableColl)
                            ->accessMethod()
                            ->initializeAsEmpty());
        wuow.commit();

        return indexBuildBlock;
    }

    /**
     * Finishes an index build that was started by createIndexWithoutFinishingBuild.
     */
    void finishIndexBuild(OperationContext* opCtx,
                          const NamespaceString& nss,
                          std::unique_ptr<IndexBuildBlock> indexBuildBlock,
                          Timestamp readyTimestamp) {
        _setupDDLOperation(opCtx, readyTimestamp);

        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        CollectionWriter collection(opCtx, nss);
        indexBuildBlock->success(opCtx, collection.getWritableCollection(opCtx));
        wuow.commit();
    }

    void concurrentCreateCollectionAndEstablishConsistentCollection(OperationContext* opCtx,
                                                                    const NamespaceString& nss,
                                                                    boost::optional<UUID> uuid,
                                                                    Timestamp timestamp,
                                                                    bool openSnapshotBeforeCommit,
                                                                    bool expectedExistence,
                                                                    int expectedNumIndexes) {
        NamespaceStringOrUUID readNssOrUUID = [&]() {
            if (uuid) {
                return NamespaceStringOrUUID(nss.dbName(), *uuid);
            } else {
                return NamespaceStringOrUUID(nss);
            }
        }();
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            readNssOrUUID,
            timestamp,
            [this, &nss, &uuid](OperationContext* opCtx) { _createCollection(opCtx, nss, uuid); },
            openSnapshotBeforeCommit,
            expectedExistence,
            expectedNumIndexes);
    }

    void concurrentCreateAndRunCatalogOperations(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<UUID> uuid,
        Timestamp timestamp,
        std::function<void(OperationContext* opCtx)> catalogOperations) {
        _concurrentDDLOperationAndCatalogQueries(
            opCtx,
            timestamp,
            [this, &nss, &uuid](OperationContext* opCtx) { _createCollection(opCtx, nss, uuid); },
            catalogOperations);
    }

    void concurrentDropCollectionAndEstablishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const NamespaceStringOrUUID& readNssOrUUID,
        Timestamp timestamp,
        bool openSnapshotBeforeCommit,
        bool expectedExistence,
        int expectedNumIndexes) {
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            readNssOrUUID,
            timestamp,
            [this, &nss, &timestamp](OperationContext* opCtx) {
                _dropCollection(opCtx, nss, timestamp);
            },
            openSnapshotBeforeCommit,
            expectedExistence,
            expectedNumIndexes);
    }

    void concurrentDropAndRunCatalogOperations(
        OperationContext* opCtx,
        const NamespaceString& nss,
        Timestamp timestamp,
        std::function<void(OperationContext* opCtx)> catalogOperations) {
        _concurrentDDLOperationAndCatalogQueries(
            opCtx,
            timestamp,
            [this, &nss, &timestamp](OperationContext* opCtx) {
                _dropCollection(opCtx, nss, timestamp);
            },
            catalogOperations);
    }

    void concurrentRenameCollectionAndEstablishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceString& from,
        const NamespaceString& to,
        const NamespaceStringOrUUID& lookupNssOrUUID,
        Timestamp timestamp,
        bool openSnapshotBeforeCommit,
        bool expectedExistence,
        int expectedNumIndexes,
        std::function<void()> verifyStateCallback = {}) {
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            lookupNssOrUUID,
            timestamp,
            [this, &from, &to, &timestamp](OperationContext* opCtx) {
                _renameCollection(opCtx, from, to, timestamp);
            },
            openSnapshotBeforeCommit,
            expectedExistence,
            expectedNumIndexes,
            std::move(verifyStateCallback));
    }

    void concurrentRenameAndRunCatalogOperations(
        OperationContext* opCtx,
        const NamespaceString& from,
        const NamespaceString& to,
        Timestamp timestamp,
        std::function<void(OperationContext* opCtx)> catalogOperations) {
        _concurrentDDLOperationAndCatalogQueries(
            opCtx,
            timestamp,
            [this, &from, &to, &timestamp](OperationContext* opCtx) {
                _renameCollection(opCtx, from, to, timestamp);
            },
            catalogOperations);
    }

    void concurrentCreateIndexAndEstablishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const NamespaceStringOrUUID& readNssOrUUID,
        BSONObj indexSpec,
        Timestamp timestamp,
        bool openSnapshotBeforeCommit,
        bool expectedExistence,
        int expectedNumIndexes,
        std::function<void(OperationContext*)> extraOpHook = {}) {
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            readNssOrUUID,
            timestamp,
            [this, &nss, &indexSpec, extraOpHook](OperationContext* opCtx) {
                _createIndex(opCtx, nss, indexSpec);
                if (extraOpHook) {
                    extraOpHook(opCtx);
                }
            },
            openSnapshotBeforeCommit,
            expectedExistence,
            expectedNumIndexes);
    }

    void concurrentDropIndexAndEstablishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const NamespaceStringOrUUID& readNssOrUUID,
        const std::string& indexName,
        Timestamp timestamp,
        bool openSnapshotBeforeCommit,
        bool expectedExistence,
        int expectedNumIndexes,
        std::function<void(OperationContext*)> extraOpHook = {}) {
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            readNssOrUUID,
            timestamp,
            [this, &nss, &indexName, extraOpHook](OperationContext* opCtx) {
                _dropIndex(opCtx, nss, indexName);
                if (extraOpHook) {
                    extraOpHook(opCtx);
                }
            },
            openSnapshotBeforeCommit,
            expectedExistence,
            expectedNumIndexes);
    }

protected:
    ServiceContext::UniqueOperationContext opCtx;

private:
    void _setupDDLOperation(OperationContext* opCtx, Timestamp timestamp) {
        RecoveryUnit* recoveryUnit = shard_role_details::getRecoveryUnit(opCtx);

        recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        recoveryUnit->abandonSnapshot();

        if (!recoveryUnit->getCommitTimestamp().isNull()) {
            recoveryUnit->clearCommitTimestamp();
        }
        recoveryUnit->setCommitTimestamp(timestamp);
    }

    UUID _createCollection(OperationContext* opCtx,
                           const NamespaceString& nss,
                           boost::optional<UUID> uuid = boost::none,
                           bool allowMixedModeWrites = false) {
        AutoGetDb databaseWriteGuard(opCtx, nss.dbName(), MODE_IX);
        auto db = databaseWriteGuard.ensureDbExists(opCtx);
        ASSERT(db);

        Lock::CollectionLock lk(opCtx, nss, MODE_IX);

        CollectionOptions options;
        if (uuid) {
            options.uuid.emplace(*uuid);
        } else {
            options.uuid.emplace(UUID::gen());
        }

        // Adds the collection to the durable catalog.
        auto storageEngine = getServiceContext()->getStorageEngine();
        const auto ident = storageEngine->generateNewCollectionIdent(nss.dbName());
        auto mdbCatalog = storageEngine->getMDBCatalog();
        const auto catalogId = mdbCatalog->reserveCatalogId(opCtx);
        auto rs = unittest::assertGet(
            durable_catalog::createCollection(opCtx, catalogId, nss, ident, options, mdbCatalog));
        auto catalogEntry = durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
        ASSERT_EQUALS(catalogEntry->catalogId, catalogId);
        ASSERT_EQUALS(catalogEntry->ident, ident);

        auto metadata = catalogEntry->metadata;
        std::shared_ptr<Collection> ownedCollection =
            Collection::Factory::get(opCtx)->make(opCtx, nss, catalogId, metadata, std::move(rs));
        ownedCollection->init(opCtx);
        invariant(ownedCollection->getSharedDecorations());
        historicalIDTrackerAllowsMixedModeWrites(ownedCollection->getSharedDecorations())
            .store(allowMixedModeWrites);

        // Adds the collection to the in-memory catalog.
        CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, std::move(ownedCollection));
        return *options.uuid;
    }

    void _dropCollection(OperationContext* opCtx, const NamespaceString& nss, Timestamp timestamp) {
        Lock::DBLock dbLk(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLk(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);

        Collection* writableCollection = collection.getWritableCollection(opCtx);

        // Drop all remaining indexes before dropping the collection.
        std::vector<std::string> indexNames;
        writableCollection->getAllIndexes(&indexNames);
        for (const auto& indexName : indexNames) {
            IndexCatalog* indexCatalog = writableCollection->getIndexCatalog();
            auto writableEntry = indexCatalog->getWritableEntryByName(
                opCtx, indexName, IndexCatalog::InclusionPolicy::kReady);

            // This also adds the index ident to the drop-pending reaper.
            ASSERT_OK(indexCatalog->dropIndexEntry(opCtx, writableCollection, writableEntry));
        }

        // Add the collection ident to the drop-pending reaper.
        opCtx->getServiceContext()->getStorageEngine()->addDropPendingIdent(
            timestamp, collection->getRecordStore()->getSharedIdent());

        // Drops the collection from the durable catalog.
        auto storageEngine = getServiceContext()->getStorageEngine();
        uassertStatusOK(durable_catalog::dropCollection(
            opCtx, writableCollection->getCatalogId(), storageEngine->getMDBCatalog()));

        // Drops the collection from the in-memory catalog.
        CollectionCatalog::get(opCtx)->dropCollection(opCtx, writableCollection);
    }

    void _renameCollection(OperationContext* opCtx,
                           const NamespaceString& from,
                           const NamespaceString& to,
                           Timestamp timestamp) {
        Lock::DBLock dbLk(opCtx, from.dbName(), MODE_IX);
        Lock::CollectionLock fromLk(opCtx, from, MODE_X);
        Lock::CollectionLock toLk(opCtx, to, MODE_X);

        // Drop the collection if it exists. This triggers the same behavior as renaming with
        // dropTarget=true.
        if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, to)) {
            _dropCollection(opCtx, to, timestamp);
        }

        CollectionWriter collection(opCtx, from);

        ASSERT_OK(collection.getWritableCollection(opCtx)->rename(opCtx, to, false));
        CollectionCatalog::get(opCtx)->onCollectionRename(
            opCtx, collection.getWritableCollection(opCtx), from);
    }

    void _createIndex(OperationContext* opCtx, const NamespaceString& nss, BSONObj indexSpec) {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            opCtx, collection, {indexSpec}, /*fromMigrate=*/false);
    }

    void _dropIndex(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const std::string& indexName) {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);

        CollectionWriter collection(opCtx, nss);

        Collection* writableCollection = collection.getWritableCollection(opCtx);

        IndexCatalog* indexCatalog = writableCollection->getIndexCatalog();
        auto writableEntry = indexCatalog->getWritableEntryByName(
            opCtx, indexName, IndexCatalog::InclusionPolicy::kReady);

        // This also adds the index ident to the drop-pending reaper.
        ASSERT_OK(indexCatalog->dropIndexEntry(opCtx, writableCollection, writableEntry));
    }

    /**
     * Simulates performing a given ddlOperation concurrently with some passed in calls to the
     * collection catalog.
     *
     * The ddl operation willl stall right after writing to the durable catalog but before
     * updating the in-memory catalog.
     */
    template <typename Callable>
    void _concurrentDDLOperationAndCatalogQueries(
        OperationContext* opCtx,
        Timestamp timestamp,
        Callable&& ddlOperation,
        std::function<void(OperationContext* opCtx)> catalogOperations) {
        stdx::mutex mutex;
        stdx::condition_variable cv;
        int numCalls = 0;

        stdx::thread t([&, svcCtx = getServiceContext()] {
            ThreadClient client(svcCtx->getService());
            auto newOpCtx = client->makeOperationContext();
            _setupDDLOperation(newOpCtx.get(), timestamp);

            WriteUnitOfWork wuow(newOpCtx.get());

            // Register a hook either preCommit or onCommit that will block until the
            // main thread has finished its openCollection lookup.
            auto commitHandler = [&]() {
                stdx::unique_lock lock(mutex);

                // Let the main thread know we have committed to the storage engine.
                numCalls = 1;
                cv.notify_all();

                // Wait until the main thread has finished its openCollection lookup.
                cv.wait(lock, [&numCalls]() { return numCalls == 2; });
            };

            class ChangeForCatalogVisibility : public RecoveryUnit::Change {
            public:
                ChangeForCatalogVisibility(std::function<void()> commitHandler)
                    : callback(std::move(commitHandler)) {}

                void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept final {
                    callback();
                }

                void rollback(OperationContext* opCtx) noexcept final {}

                std::function<void()> callback;
            };

            shard_role_details::getRecoveryUnit(newOpCtx.get())
                ->registerChangeForCatalogVisibility(
                    std::make_unique<ChangeForCatalogVisibility>(commitHandler));

            ddlOperation(newOpCtx.get());

            wuow.commit();
        });

        // Wait for the thread above to start its commit of the DDL operation.
        {
            stdx::unique_lock lock(mutex);
            cv.wait(lock, [&numCalls]() { return numCalls == 1; });
        }

        catalogOperations(opCtx);

        // Notify the thread that our openCollection lookup is done.
        {
            stdx::unique_lock lock(mutex);
            numCalls = 2;
            cv.notify_all();
        }
        t.join();
    }

    /**
     * Simulates performing a given ddlOperation concurrently with an untimestamped openCollection
     * lookup.
     *
     * If openSnapshotBeforeCommit is true, the ddlOperation stalls right after the catalog places
     * the collection in _pendingCommitNamespaces but before writing to the durable catalog.
     * Otherwise, the ddlOperation stalls right after writing to the durable catalog but before
     * updating the in-memory catalog.
     */
    template <typename Callable>
    void _concurrentDDLOperationAndEstablishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        Timestamp timestamp,
        Callable&& ddlOperation,
        bool openSnapshotBeforeCommit,
        bool expectedExistence,
        int expectedNumIndexes,
        std::function<void()> verifyStateCallback = {}) {
        stdx::mutex mutex;
        stdx::condition_variable cv;
        int numCalls = 0;

        stdx::thread t([&, svcCtx = getServiceContext()] {
            ThreadClient client(svcCtx->getService());
            auto newOpCtx = client->makeOperationContext();
            _setupDDLOperation(newOpCtx.get(), timestamp);

            WriteUnitOfWork wuow(newOpCtx.get());

            // Register a hook either preCommit or onCommit that will block until the
            // main thread has finished its openCollection lookup.
            auto commitHandler = [&]() {
                stdx::unique_lock lock(mutex);

                // Let the main thread know we have committed to the storage engine.
                numCalls = 1;
                cv.notify_all();

                // Wait until the main thread has finished its openCollection lookup.
                cv.wait(lock, [&numCalls]() { return numCalls == 2; });
            };

            // The onCommit handler must be registered prior to the DDL operation so it's executed
            // before any onCommit handlers set up in the operation.
            if (!openSnapshotBeforeCommit) {
                // Need to use 'registerChangeForCatalogVisibility' so it can happen after storage
                // engine commit but before the changes become visible in the catalog.
                class ChangeForCatalogVisibility : public RecoveryUnit::Change {
                public:
                    ChangeForCatalogVisibility(std::function<void()> commitHandler)
                        : callback(std::move(commitHandler)) {}

                    void commit(OperationContext* opCtx,
                                boost::optional<Timestamp>) noexcept final {
                        callback();
                    }

                    void rollback(OperationContext* opCtx) noexcept final {}

                    std::function<void()> callback;
                };

                shard_role_details::getRecoveryUnit(newOpCtx.get())
                    ->registerChangeForCatalogVisibility(
                        std::make_unique<ChangeForCatalogVisibility>(commitHandler));
            }

            ddlOperation(newOpCtx.get());

            // The preCommit handler must be registered after the DDL operation so it's executed
            // after any preCommit hooks set up in the operation.
            if (openSnapshotBeforeCommit) {
                shard_role_details::getRecoveryUnit(newOpCtx.get())
                    ->registerPreCommitHook(
                        [&commitHandler](OperationContext* opCtx) { commitHandler(); });
            }

            wuow.commit();
        });

        // Wait for the thread above to start its commit of the DDL operation.
        {
            stdx::unique_lock lock(mutex);
            cv.wait(lock, [&numCalls]() { return numCalls == 1; });
        }

        // Perform the openCollection lookup.
        OneOffRead oor(opCtx, Timestamp());
        Lock::GlobalLock globalLock(opCtx, MODE_IS);
        // Stash the catalog so we may perform multiple lookups that will be in sync with our
        // snapshot
        CollectionCatalog::stash(opCtx, CollectionCatalog::get(opCtx));
        auto coll = CollectionCatalog::get(opCtx)->establishConsistentCollection(
            opCtx, nssOrUUID, boost::none);

        // Notify the thread that our openCollection lookup is done.
        {
            stdx::unique_lock lock(mutex);
            numCalls = 2;
            cv.notify_all();
        }
        t.join();


        auto catalog = CollectionCatalog::get(opCtx);
        auto mdbCatalog = MDBCatalog::get(opCtx);
        if (expectedExistence) {
            ASSERT(coll);

            NamespaceString nss = catalog->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);

            ASSERT_EQ(coll->ns(), nss);
            // Check that lookup returns the same instance as openCollection above
            ASSERT_EQ(catalog->lookupCollectionByNamespace(opCtx, coll->ns()), coll.get());
            ASSERT_EQ(catalog->lookupCollectionByUUID(opCtx, coll->uuid()), coll.get());
            ASSERT_EQ(catalog->lookupNSSByUUID(opCtx, coll->uuid()), nss);
            ASSERT_EQ(coll->getIndexCatalog()->numIndexesTotal(), expectedNumIndexes);

            auto catalogEntry =
                durable_catalog::getParsedCatalogEntry(opCtx, coll->getCatalogId(), mdbCatalog);
            ASSERT(catalogEntry);
            ASSERT(coll->isMetadataEqual(catalogEntry->metadata->toBSON()));

            // Lookups from the catalog should return the newly opened collection.
            ASSERT_EQ(catalog->lookupCollectionByNamespace(opCtx, coll->ns()), coll.get());
            ASSERT_EQ(catalog->lookupCollectionByUUID(opCtx, coll->uuid()), coll.get());
        } else {
            ASSERT(!coll);
            if (nssOrUUID.isNamespaceString()) {
                auto catalogEntry =
                    durable_catalog::scanForCatalogEntryByNss(opCtx, nssOrUUID.nss(), mdbCatalog);
                ASSERT(!catalogEntry);

                // Lookups from the catalog should return the newly opened collection (in this case
                // nullptr).
                ASSERT_EQ(catalog->lookupCollectionByNamespace(opCtx, nssOrUUID.nss()), coll.get());
            } else {
                auto catalogEntry =
                    durable_catalog::scanForCatalogEntryByUUID(opCtx, nssOrUUID.uuid(), mdbCatalog);
                ASSERT(!catalogEntry);

                // Lookups from the catalog should return the newly opened collection (in this case
                // nullptr).
                ASSERT_EQ(catalog->lookupCollectionByUUID(opCtx, nssOrUUID.uuid()), coll.get());
            }
        }

        if (verifyStateCallback) {
            verifyStateCallback();
        }
    }
};

class CollectionCatalogNoTimestampTest : public CollectionCatalogTimestampTest {
public:
    CollectionCatalogNoTimestampTest()
        : CollectionCatalogTimestampTest(CollectionCatalogTimestampTest::DisableTimestampingTag{}) {
    }
};

TEST_F(CollectionCatalogTimestampTest, MinimumValidSnapshot) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);
    const Timestamp dropIndexTs = Timestamp(40, 40);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createYIndexTs);

    auto coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
    ASSERT(coll);
    ASSERT_EQ(coll->getMinimumValidSnapshot(), createYIndexTs);

    dropIndex(opCtx.get(), nss, "x_1", dropIndexTs);
    dropIndex(opCtx.get(), nss, "y_1", dropIndexTs);

    // Fetch the latest collection instance without the indexes.
    coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
    ASSERT(coll);
    ASSERT_EQ(coll->getMinimumValidSnapshot(), dropIndexTs);
}

TEST_F(CollectionCatalogTimestampTest, OpenCollectionBeforeCreateTimestamp) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    createCollection(opCtx.get(), nss, createCollectionTs);

    // Try to open the collection before it was created.
    const Timestamp readTimestamp(5, 5);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    OneOffRead oor(opCtx.get(), readTimestamp);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(!coll);

    // Lookups from the catalog should return the newly opened collection (in this case nullptr).
    ASSERT_EQ(CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss),
              coll.get());
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollection) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);

    // Open an instance of the collection before the index was created.
    const Timestamp readTimestamp(15, 15);
    OneOffRead oor(opCtx.get(), readTimestamp);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);
    ASSERT_EQ(0, coll->getIndexCatalog()->numIndexesTotal());

    // Verify that the CollectionCatalog returns the latest collection with the index present. This
    // has to be done in an alternative client as we already have an open snapshot from an earlier
    // point-in-time above.
    auto newClient = opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);
    ASSERT(latestColl);
    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesTotal());

    // Ensure the idents are shared between the collection instances.
    ASSERT_NE(coll.get(), latestColl);
    ASSERT_EQ(coll->getSharedIdent(), latestColl->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollectionWithIndex) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createYIndexTs);

    // Open an instance of the collection when only one of the two indexes were present.
    const Timestamp readTimestamp(25, 25);
    OneOffRead oor(opCtx.get(), readTimestamp);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);
    ASSERT_EQ(1, coll->getIndexCatalog()->numIndexesTotal());

    // Verify that the CollectionCatalog returns the latest collection. This has to be done in an
    // alternative client as we already have an open snapshot from an earlier point-in-time above.
    auto newClient = opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);
    ASSERT(latestColl);
    ASSERT_EQ(2, latestColl->getIndexCatalog()->numIndexesTotal());

    // Ensure the idents are shared between the collection and index instances.
    ASSERT_NE(coll.get(), latestColl);
    ASSERT_EQ(coll->getSharedIdent(), latestColl->getSharedIdent());

    auto indexDescPast = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescLatest = latestColl->getIndexCatalog()->findIndexByName(newOpCtx.get(), "x_1");
    ASSERT_BSONOBJ_EQ(indexDescPast->infoObj(), indexDescLatest->infoObj());
    ASSERT_EQ(coll->getIndexCatalog()->getEntryShared(indexDescPast)->getSharedIdent(),
              latestColl->getIndexCatalog()->getEntryShared(indexDescLatest)->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenLatestCollectionWithIndex) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    // Setting the read timestamp to the last DDL operation on the collection returns the latest
    // collection.
    const Timestamp readTimestamp(20, 20);
    OneOffRead oor(opCtx.get(), readTimestamp);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);

    // Verify that the CollectionCatalog returns the latest collection.
    auto currentColl =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
    ASSERT_EQ(coll.get(), currentColl);

    // Ensure the idents are shared between the collection and index instances.
    ASSERT_EQ(coll->getSharedIdent(), currentColl->getSharedIdent());

    auto indexDesc = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescCurrent = currentColl->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    ASSERT_BSONOBJ_EQ(indexDesc->infoObj(), indexDescCurrent->infoObj());
    ASSERT_EQ(coll->getIndexCatalog()->getEntryShared(indexDesc)->getSharedIdent(),
              currentColl->getIndexCatalog()->getEntryShared(indexDescCurrent)->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollectionWithDropPendingIndex) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    // Maintain a shared_ptr to "x_1", so it's not expired in drop pending map, but not for "y_1".
    std::shared_ptr<const IndexCatalogEntry> index;
    {
        auto latestColl =
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
        auto desc = latestColl->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
        index = latestColl->getIndexCatalog()->getEntryShared(desc);
    }

    dropIndex(opCtx.get(), nss, "x_1", dropIndexTs);
    dropIndex(opCtx.get(), nss, "y_1", dropIndexTs);

    // Open the collection while both indexes were present.
    const Timestamp readTimestamp(20, 20);
    OneOffRead oor(opCtx.get(), readTimestamp);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);
    ASSERT_EQ(coll->getIndexCatalog()->numIndexesReady(), 2);

    // Collection is not shared from the latest instance. This has to be done in an  alternative
    // client as we already have an open snapshot from an earlier point-in-time above.
    auto newClient = opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);
    ASSERT_NE(coll.get(), latestColl);

    auto indexDescX = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescY = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "y_1");

    auto indexEntryX = coll->getIndexCatalog()->getEntryShared(indexDescX);
    auto indexEntryXIdent = indexEntryX->getSharedIdent();
    auto indexEntryYIdent = coll->getIndexCatalog()->getEntryShared(indexDescY)->getSharedIdent();

    // Check use_count(). 2 in the unit test, 1 in the opened collection.
    ASSERT_EQ(3, indexEntryXIdent.use_count());

    // Check use_count(). 1 in the unit test, 1 in the opened collection.
    ASSERT_EQ(2, indexEntryYIdent.use_count());

    // Verify that "x_1"'s ident was retrieved from the drop pending map for the opened collection.
    ASSERT_EQ(index->getSharedIdent(), indexEntryXIdent);
}

TEST_F(CollectionCatalogTimestampTest,
       OpenEarlierCollectionWithDropPendingIndexDoesNotCrashWhenCheckingMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    const std::string xIndexName{"x_1"};
    const std::string yIndexName{"y_1"};
    const std::string zIndexName{"z_1"};

    const Timestamp createCollectionTs = Timestamp(10, 10);

    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(21, 21);
    const Timestamp createZIndexTs = Timestamp(22, 22);

    const Timestamp dropYIndexTs = Timestamp(30, 30);
    const Timestamp tsBetweenDroppingYAndZ = Timestamp(31, 31);
    const Timestamp dropZIndexTs = Timestamp(33, 33);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name" << xIndexName << "key" << BSON("x" << 1)),
                createXIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name" << yIndexName << "key" << BSON("y" << 1)),
                createYIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name" << zIndexName << "key" << BSON("z" << 1)),
                createZIndexTs);

    // Maintain a shared_ptr to "z_1", so it's not expired in drop pending map. This is required so
    // that this index entry's ident will be re-used when openCollection is called.
    std::shared_ptr<const IndexCatalogEntry> index = [&] {
        auto latestColl =
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
        auto desc = latestColl->getIndexCatalog()->findIndexByName(opCtx.get(), zIndexName);
        return latestColl->getIndexCatalog()->getEntryShared(desc);
    }();

    dropIndex(opCtx.get(), nss, yIndexName, dropYIndexTs);
    dropIndex(opCtx.get(), nss, zIndexName, dropZIndexTs);

    // Open the collection after the first index drop but before the second. This ensures we get a
    // version of the collection whose indexes are {x, z} in the durable catalog, while the
    // metadata for the in-memory latest collection contains indexes {x, {}, {}} (where {}
    // corresponds to a default-constructed object). The index catalog entry for the z index will be
    // contained in the drop pending reaper. So the CollectionImpl object created by openCollection
    // will reuse index idents for indexes x and z.
    //
    // This test originally reproduced a bug where:
    //     * The index catalog entry object for z contained an _indexOffset of 2, because of its
    //       location in the latest catalog entry's metadata.indexes array
    //     * openCollection would re-use the index catalog entry for z (with _indexOffset=2), but
    //       it would store this entry at position 1 in its metadata.indexes array
    //     * Something would try to check if the index was multikey, and it would use the offset of
    //       2 contained in the IndexCatalogEntry, but this was incorrect for the CollectionImpl
    //       object, so it would fire an invariant.
    const Timestamp readTimestamp = tsBetweenDroppingYAndZ;
    OneOffRead oor(opCtx.get(), readTimestamp);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);
    ASSERT_EQ(coll->getIndexCatalog()->numIndexesReady(), 2);

    // Collection is not shared from the latest instance. This has to be done in an  alternative
    // client as we already have an open snapshot from an earlier point-in-time above.
    auto newClient = opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);

    ASSERT_NE(coll.get(), latestColl);

    auto indexDescZ = coll->getIndexCatalog()->findIndexByName(opCtx.get(), zIndexName);
    auto indexEntryZ = coll->getIndexCatalog()->getEntryShared(indexDescZ);
    auto indexEntryZIsMultikey = indexEntryZ->isMultikey(newOpCtx.get(), CollectionPtr(coll));

    ASSERT_FALSE(indexEntryZIsMultikey);
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierAlreadyDropPendingCollection) {
    const NamespaceString firstNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString secondNss = NamespaceString::createNamespaceString_forTest("c.d");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(30, 30);

    createCollection(opCtx.get(), firstNss, createCollectionTs);
    createCollection(opCtx.get(), secondNss, createCollectionTs);

    // Maintain a shared_ptr to the catalog so that collection "a.b" isn't expired in the drop
    // pending map after we drop the collections.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto coll = catalog->lookupCollectionByNamespace(opCtx.get(), firstNss);
    ASSERT(coll);

    // Make the collections drop pending.
    dropCollection(opCtx.get(), firstNss, dropCollectionTs);
    dropCollection(opCtx.get(), secondNss, dropCollectionTs);

    // Set the read timestamp to be before the drop timestamp.
    const Timestamp readTimestamp(20, 20);

    {
        OneOffRead oor(opCtx.get(), readTimestamp);

        // Open "a.b", which is not expired in the drop pending map.
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), firstNss, readTimestamp);
        ASSERT(openedColl);
        ASSERT_EQ(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), firstNss),
            openedColl.get());

        // Release the consistent collection reference so we can advance the snapshot.
        auto ptr = openedColl.get();
        openedColl = ConsistentCollection{};
        shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();

        // Once snapshot is abandoned, openedColl has been released so it should not match the
        // collection lookup.
        ASSERT_NE(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), firstNss),
            ptr);
    }

    {
        OneOffRead oor(opCtx.get(), readTimestamp);

        // Open "c.d" which is expired in the drop pending map.
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        // Before openCollection, looking up the collection returns null.
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), secondNss));
        auto openedColl =
            CollectionCatalog::get(opCtx.get())
                ->establishConsistentCollection(opCtx.get(), secondNss, readTimestamp);
        ASSERT(openedColl);
        ASSERT_EQ(CollectionCatalog::get(opCtx.get())
                      ->lookupCollectionByNamespace(opCtx.get(), secondNss),
                  openedColl.get());
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionUsingDropPendingCollectionSharedState) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropCollectionTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);

    // Maintain a shared_ptr to the catalog so that the collection "a.b" isn't expired in the drop
    // pending map after we drop it.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto coll = catalog->lookupCollectionByNamespace(opCtx.get(), nss);

    ASSERT(coll);
    ASSERT_EQ(coll->getMinimumValidSnapshot(), createIndexTs);

    // Make the collection drop pending.
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    // Open the collection before the index was created. The drop pending collection is incompatible
    // as it has an index entry. But we can still use the drop pending collections shared state to
    // instantiate a new collection.
    const Timestamp readTimestamp(10, 10);
    OneOffRead oor(opCtx.get(), readTimestamp);

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    auto openedColl = CollectionCatalog::get(opCtx.get())
                          ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(openedColl);
    ASSERT_NE(coll, openedColl.get());
    // Ensure the idents are shared between the opened collection and the drop pending collection.
    ASSERT_EQ(coll->getSharedIdent(), openedColl->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, checkIfUUIDExistsAtLatest) {
    // A "committed on disk" collection UUID can be retrieved from the latest catalog snapshot.
    {
        const NamespaceString nssOnCatalog =
            NamespaceString::createNamespaceString_forTest("a.committed");
        const Timestamp createCollectionTs = Timestamp(10, 10);
        const auto collUuidOnCatalog =
            createCollection(opCtx.get(), nssOnCatalog, createCollectionTs);

        auto catalog = CollectionCatalog::get(opCtx.get());
        auto match = catalog->checkIfUUIDExistsAtLatest(opCtx.get(), collUuidOnCatalog);
        ASSERT_TRUE(match);
    }

    // The UUID of an uncommitted createCollection can be retrieved from the open storage
    // transaction.
    {
        const NamespaceString nssOnCatalog =
            NamespaceString::createNamespaceString_forTest("a.uncommitted");
        const Timestamp createCollectionTs = Timestamp(10, 10);
        const UUID uncommittedCollUUID = UUID::gen();
        boost::optional<WriteUnitOfWork> wuow;
        createCollectionWithUUIDAndLeaveUncommitted(
            opCtx.get(), nssOnCatalog, createCollectionTs, uncommittedCollUUID, wuow);

        auto catalog = CollectionCatalog::get(opCtx.get());
        auto match = catalog->checkIfUUIDExistsAtLatest(opCtx.get(), uncommittedCollUUID);
        ASSERT_TRUE(match);
        wuow->commit();
    }

    // The UUID of a commit-pending createCollection can be retrieved from the related data
    // structure.
    {
        const NamespaceString commitPendingNss =
            NamespaceString::createNamespaceString_forTest("a.createWithCommitPending");
        const UUID commitPendingColluuid = UUID::gen();
        const Timestamp createCollectionTs = Timestamp(10, 10);

        concurrentCreateAndRunCatalogOperations(
            opCtx.get(),
            commitPendingNss,
            commitPendingColluuid,
            createCollectionTs,
            [this, &commitPendingColluuid](OperationContext* opCtx) {
                auto catalog = CollectionCatalog::get(opCtx);
                auto match = catalog->checkIfUUIDExistsAtLatest(opCtx, commitPendingColluuid);
                ASSERT_TRUE(match);
            });
    }

    // The UUID of a commit-pending dropped collection can still be retrieved from the latest
    // catalog snapshot.
    {
        const NamespaceString nssOnCatalog =
            NamespaceString::createNamespaceString_forTest("a.dropWithCommitPending");
        const Timestamp createCollectionTs = Timestamp(10, 10);
        const Timestamp dropCollectionTs = Timestamp(20, 20);

        const auto collUuidOnCatalog =
            createCollection(opCtx.get(), nssOnCatalog, createCollectionTs);

        concurrentDropAndRunCatalogOperations(opCtx.get(),
                                              nssOnCatalog,
                                              dropCollectionTs,
                                              [this, &collUuidOnCatalog](OperationContext* opCtx) {
                                                  auto catalog = CollectionCatalog::get(opCtx);
                                                  auto match = catalog->checkIfUUIDExistsAtLatest(
                                                      opCtx, collUuidOnCatalog);
                                                  ASSERT_TRUE(match);
                                              });
    }

    // A collection being dropped within the currently open storage transaction can still be
    // retrieved through the latest catalog snapshot.
    {
        const NamespaceString nssOnCatalog =
            NamespaceString::createNamespaceString_forTest("a.toBeDroppedInOpenTxn");
        const Timestamp createCollectionTs = Timestamp(10, 10);
        const Timestamp dropCollectionTs = Timestamp(20, 20);

        const auto collUuidOnCatalog =
            createCollection(opCtx.get(), nssOnCatalog, createCollectionTs);

        boost::optional<WriteUnitOfWork> wuow;

        dropCollectionAndLeaveUncommitted(opCtx.get(), nssOnCatalog, dropCollectionTs, wuow);

        auto catalog = CollectionCatalog::get(opCtx.get());
        auto match = catalog->checkIfUUIDExistsAtLatest(opCtx.get(), collUuidOnCatalog);
        ASSERT_TRUE(match);
        wuow->commit();
    }

    // A non-existing collection UUID matches no collection.
    {
        const UUID nonExistingUuid = UUID::gen();
        auto catalog = CollectionCatalog::get(opCtx.get());
        auto match = catalog->checkIfUUIDExistsAtLatest(opCtx.get(), nonExistingUuid);
        ASSERT_FALSE(match);
    }
}

TEST_F(CollectionCatalogTimestampTest, CollectionLifetimeTiedToStorageTransactionLifetime) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);

    const Timestamp readTimestamp(15, 15);

    {
        // Test that the collection is released when the storage snapshot is abandoned.
        OneOffRead oor(opCtx.get(), readTimestamp);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        {
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
            ASSERT(coll);

            std::shared_ptr<const Collection> fetchedColl =
                OpenedCollections::get(opCtx.get()).lookupByNamespace(nss).value();
            ASSERT(fetchedColl);
            ASSERT_EQ(coll.get(), fetchedColl.get());
            ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());
        }
        shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));
    }

    {
        // Test that the collection is released when the storage snapshot is committed.
        OneOffRead oor(opCtx.get(), readTimestamp);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        auto coll = CollectionCatalog::get(opCtx.get())
                        ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
        ASSERT(coll);

        WriteUnitOfWork wuow(opCtx.get());

        std::shared_ptr<const Collection> fetchedColl =
            OpenedCollections::get(opCtx.get()).lookupByNamespace(nss).value();
        ASSERT(fetchedColl);
        ASSERT_EQ(coll.get(), fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        coll = ConsistentCollection{};

        wuow.commit();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));

        shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));
    }

    {
        // Test that the collection is released when the storage snapshot is aborted.
        OneOffRead oor(opCtx.get(), readTimestamp);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        auto coll = CollectionCatalog::get(opCtx.get())
                        ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
        ASSERT(coll);

        boost::optional<WriteUnitOfWork> wuow(opCtx.get());

        std::shared_ptr<const Collection> fetchedColl =
            OpenedCollections::get(opCtx.get()).lookupByNamespace(nss).value();
        ASSERT(fetchedColl);
        ASSERT_EQ(coll.get(), fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        coll = ConsistentCollection{};

        // The storage snapshot is aborted when the WriteUnitOfWork destructor runs.
        wuow.reset();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));

        shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));
    }
}

TEST_F(CollectionCatalogTimestampTest, EstablishConsistentCollectionReadYourWrites) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const UUID expectedUUID = UUID::gen();

    boost::optional<WriteUnitOfWork> wuow;
    createCollectionWithUUIDAndLeaveUncommitted(
        opCtx.get(), nss, createCollectionTs, expectedUUID, wuow);

    auto coll =
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, boost::none /* readTimestamp */);
    ASSERT(coll);
    ASSERT_EQ(expectedUUID, coll->uuid());

    wuow->commit();
}

TEST_F(CollectionCatalogTimestampTest, EstablishConsistentCollectionReadYourWritesChildWUOW) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const UUID expectedUUID = UUID::gen();

    boost::optional<WriteUnitOfWork> wuow;
    createCollectionWithUUIDAndCommitInNestedUnitOnly(
        opCtx.get(), nss, createCollectionTs, expectedUUID, wuow);

    auto coll =
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, boost::none /* readTimestamp */);
    ASSERT(coll);
    ASSERT_EQ(expectedUUID, coll->uuid());

    wuow->commit();
}

// Regression test for SERVER-108988:
// (1) Operation 1 starts a WriteUnitOfWork.
// (2) Operation 1 starts a nested WriteUnitOfWork, creates a collection and commits it.
//     (Note that this does not really commit until the toplevel WriteUnitOfWork commits.)
// (3) Concurrently, operation 2 is committing that same collection.
// (4) Operation 1 tries to read the collection using EstablishConsistentCollection.
//
// In this scenario, (4) should read the instance of the collection created by (2), as we provide
// "read your writes" consistency, even though Operation 1 ultimately fails to commit.
TEST_F(CollectionCatalogTimestampTest,
       EstablishConsistentCollectionReadYourWritesChildWUOWWithWriteConflict) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    concurrentCreateAndRunCatalogOperations(
        opCtx.get(), nss, UUID::gen(), createCollectionTs, [&](OperationContext* opCtx) {
            const UUID expectedUUID = UUID::gen();

            // Create the collection in a nested WriteUnitOfWork, so it's not yet committed
            boost::optional<WriteUnitOfWork> wuow;
            createCollectionWithUUIDAndCommitInNestedUnitOnly(
                opCtx, nss, createCollectionTs, expectedUUID, wuow);

            // Establish the collection and assert we get the one we just created in the child
            // WriteUnitOfWork (not the one which is being concurrently committed)
            auto coll = CollectionCatalog::get(opCtx)->establishConsistentCollection(
                opCtx, nss, boost::none /* readTimestamp */);
            ASSERT(coll);
            ASSERT_EQ(expectedUUID, coll->uuid());

            // The concurrent operation is already committing that namespace, so we fail to commit.
            ASSERT_THROWS(wuow->commit(), WriteConflictException);
        });
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST_F(CollectionCatalogTimestampTest,
             DetectLeftoverOpenCollections,
             "Server encountered potential use-after free") {
    const NamespaceString collNss = NamespaceString::createNamespaceString_forTest("a.coll");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    // We put this line here in order to make it the last to be destroyed.
    ConsistentCollection collection;

    createCollection(opCtx.get(), collNss, createCollectionTs);

    const Timestamp readTimestamp(15, 15);

    // This will leave a stale reference to the Collection once we abandon the snapshot during
    // destruction of the GlobalLock.
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    collection = CollectionCatalog::get(opCtx.get())
                     ->establishConsistentCollection(opCtx.get(), collNss, readTimestamp);
}
#endif

DEATH_TEST_F(CollectionCatalogTimestampTest, OpenCollectionInWriteUnitOfWork, "invariant") {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);

    const Timestamp readTimestamp(15, 15);

    WriteUnitOfWork wuow(opCtx.get());

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    CollectionCatalog::get(opCtx.get())
        ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateCollectionAndOpenCollectionBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    // When the snapshot is opened right before the create is committed to the durable catalog, the
    // collection instance should not exist yet.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, boost::none, createCollectionTs, true, false, 0);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateCollectionAndOpenCollectionAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    // When the snapshot is opened right after the create is committed to the durable catalog, the
    // collection instance should exist.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, boost::none, createCollectionTs, false, true, 0);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateCollectionAndOpenCollectionByUUIDBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    UUID uuid = UUID::gen();

    // When the snapshot is opened right before the create is committed to the durable catalog, the
    // collection instance should not exist yet.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, uuid, createCollectionTs, true, false, 0);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateCollectionAndOpenCollectionByUUIDAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    UUID uuid = UUID::gen();

    // When the snapshot is opened right after the create is committed to the durable catalog, the
    // collection instance should exist.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, uuid, createCollectionTs, false, true, 0);
}

TEST_F(CollectionCatalogTimestampTest, UUIDLookupWhileCommitPendingCreate) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    UUID uuid = UUID::gen();

    concurrentCreateAndRunCatalogOperations(
        opCtx.get(), nss, uuid, createCollectionTs, [this, &nss, &uuid](OperationContext* opCtx) {
            auto catalog = CollectionCatalog::get(opCtx);
            ASSERT_THROWS(catalog->resolveNamespaceStringOrUUID(opCtx, {nss.dbName(), uuid}),
                          ExceptionFor<ErrorCodes::NamespaceNotFound>);
            ASSERT_EQ(catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                          opCtx, {nss.dbName(), uuid}),
                      nss);
        });
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropCollectionAndOpenCollectionBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);

    // When the snapshot is opened right before the drop is committed to the durable catalog, the
    // collection instance should be returned.
    concurrentDropCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, nss, dropCollectionTs, true, true, 0);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropCollectionAndOpenCollectionAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);

    // When the snapshot is opened right after the drop is committed to the durable catalog, no
    // collection instance should be returned.
    concurrentDropCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, nss, dropCollectionTs, false, false, 0);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentDropCollectionAndOpenCollectionByUUIDBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the drop is committed to the durable catalog, the
    // collection instance should be returned.
    concurrentDropCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, uuidWithDbName, dropCollectionTs, true, true, 0);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropCollectionAndOpenCollectionByUUIDAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right after the drop is committed to the durable catalog, no
    // collection instance should be returned.
    concurrentDropCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, uuidWithDbName, dropCollectionTs, false, false, 0);
}

TEST_F(CollectionCatalogTimestampTest, UUIDLookupWhileCommitPendingDrop) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();

    concurrentDropAndRunCatalogOperations(
        opCtx.get(), nss, dropCollectionTs, [this, &nss, &uuid](OperationContext* opCtx) {
            auto catalog = CollectionCatalog::get(opCtx);
            ASSERT_EQ(catalog->resolveNamespaceStringOrUUID(opCtx, {nss.dbName(), uuid}), nss);
            ASSERT_EQ(catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                          opCtx, {nss.dbName(), uuid}),
                      nss);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithOriginalNameBeforeCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);

    // When the snapshot is opened right before the rename is committed to the durable catalog, and
    // the openCollection looks for the originalNss, the collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, newNss, originalNss, renameCollectionTs, true, true, 0);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithOriginalNameAfterCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();

    // When the snapshot is opened right after the rename is committed to the durable catalog, and
    // the openCollection looks for the originalNss, no collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, newNss, originalNss, renameCollectionTs, false, false, 0, [&]() {
            // Verify that we can find the Collection when we search by UUID when the setup occured
            // during concurrent rename (rename is not affecting UUID), even if we can't find it by
            // namespace.
            auto coll =
                CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), uuid);
            ASSERT(coll);
            ASSERT_EQ(coll->ns(), newNss);

            ASSERT_EQ(CollectionCatalog::get(opCtx.get())->lookupNSSByUUID(opCtx.get(), uuid),
                      newNss);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithNewNameBeforeCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();

    // When the snapshot is opened right before the rename is committed to the durable catalog, and
    // the openCollection looks for the newNss, no collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, newNss, newNss, renameCollectionTs, true, false, 0, [&]() {
            // Verify that we can find the Collection when we search by UUID when the setup occured
            // during concurrent rename (rename is not affecting UUID), even if we can't find it by
            // namespace.
            auto coll =
                CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), uuid);
            ASSERT(coll);
            ASSERT_EQ(coll->ns(), originalNss);

            ASSERT_EQ(CollectionCatalog::get(opCtx.get())->lookupNSSByUUID(opCtx.get(), uuid),
                      originalNss);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithNewNameAfterCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);

    // When the snapshot is opened right after the rename is committed to the durable catalog, and
    // the openCollection looks for the newNss, the collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, newNss, newNss, renameCollectionTs, false, true, 0);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithUUIDBeforeCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();
    NamespaceStringOrUUID uuidWithDbName(originalNss.dbName(), uuid);
    // When the snapshot is opened right before the rename is committed to the durable catalog, and
    // the openCollection looks for the originalNss, the collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, newNss, uuidWithDbName, renameCollectionTs, true, true, 0, [&]() {
            // Verify that we cannot find the Collection when we search by the new namespace as
            // the rename was committed when we read.
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), newNss);
            ASSERT(!coll);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithUUIDAfterCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();
    NamespaceStringOrUUID uuidWithDbName(originalNss.dbName(), uuid);

    // When the snapshot is opened right after the rename is committed to the durable catalog, and
    // the openCollection looks for the originalNss, no collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(),
        originalNss,
        newNss,
        uuidWithDbName,
        renameCollectionTs,
        false,
        true,
        0,
        [&]() {
            // Verify that we cannot find the Collection
            // when we search by the original namespace as
            // the rename was committed when we read.
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss);
            ASSERT(!coll);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionBeforeCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    // We expect to find the UUID for the original collection
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();

    // When the snapshot is opened right before the rename is committed to the durable catalog, and
    // the openCollection looks for the targetNss, we find the target collection.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, targetNss, targetNss, renameCollectionTs, true, true, 0, [&]() {
            // Verify that we can find the original Collection when we search by original UUID.
            auto coll =
                CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), uuid);
            ASSERT(coll);
            ASSERT_EQ(coll->ns(), originalNss);

            ASSERT_EQ(CollectionCatalog::get(opCtx.get())->lookupNSSByUUID(opCtx.get(), uuid),
                      originalNss);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionAfterCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    // We expect to find the UUID for the original collection
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();
    UUID uuidDropped = CollectionCatalog::get(opCtx.get())
                           ->lookupCollectionByNamespace(opCtx.get(), targetNss)
                           ->uuid();

    // When the snapshot is opened right after the rename is committed to the durable catalog, and
    // the openCollection looks for the targetNss, we find the original collection.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(), originalNss, targetNss, targetNss, renameCollectionTs, false, true, 0, [&]() {
            // Verify that search by UUID is as expected and returns the target collection
            auto coll =
                CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), uuid);
            ASSERT(coll);
            ASSERT_EQ(coll->ns(), targetNss);
            ASSERT(!CollectionCatalog::get(opCtx.get())
                        ->lookupCollectionByUUID(opCtx.get(), uuidDropped));

            ASSERT_EQ(CollectionCatalog::get(opCtx.get())->lookupNSSByUUID(opCtx.get(), uuid),
                      targetNss);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionWithOriginalUUIDBeforeCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    // We expect to find the UUID for the original collection
    UUID originalUUID = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                            ->uuid();
    UUID targetUUID = CollectionCatalog::get(opCtx.get())
                          ->lookupCollectionByNamespace(opCtx.get(), targetNss)
                          ->uuid();
    NamespaceStringOrUUID uuidWithDbName(originalNss.dbName(), originalUUID);

    // When the snapshot is opened right before the rename is committed to the durable catalog, and
    // the openCollection looks for the original UUID, we should find the original collection
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(),
        originalNss,
        targetNss,
        uuidWithDbName,
        renameCollectionTs,
        true,
        true,
        0,
        [&]() {
            // Verify that we can find the original Collection when we search by namespace as rename
            // has not committed yet.
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss);
            ASSERT(coll);
            ASSERT_EQ(coll->uuid(), originalUUID);

            // Verify that we can find the target Collection when we search by namespace as rename
            // has not committed yet.
            coll = CollectionCatalog::get(opCtx.get())
                       ->lookupCollectionByNamespace(opCtx.get(), targetNss);
            ASSERT(coll);
            ASSERT_EQ(coll->uuid(), targetUUID);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionWithOriginalUUIDAfterCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    // We expect to find the UUID for the original collection
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();
    NamespaceStringOrUUID uuidWithDbName(originalNss.dbName(), uuid);

    // When the snapshot is opened right after the rename is committed to the durable catalog, and
    // the openCollection looks for the newNss, no collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(),
        originalNss,
        targetNss,
        uuidWithDbName,
        renameCollectionTs,
        false,
        true,
        0,
        [&]() {
            // Verify that we cannot find the Collection when we search by the original namespace.
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss);
            ASSERT(!coll);

            // Verify that we can find the original Collection UUID when we search by namespace.
            coll = CollectionCatalog::get(opCtx.get())
                       ->lookupCollectionByNamespace(opCtx.get(), targetNss);
            ASSERT(coll);
            ASSERT_EQ(coll->uuid(), uuid);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionWithTargetUUIDBeforeCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    UUID originalUUID = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                            ->uuid();
    UUID targetUUID = CollectionCatalog::get(opCtx.get())
                          ->lookupCollectionByNamespace(opCtx.get(), targetNss)
                          ->uuid();
    NamespaceStringOrUUID uuidWithDbName(originalNss.dbName(), targetUUID);

    // When the snapshot is opened right before the rename is committed to the durable catalog, and
    // the openCollection looks for the original UUID, we should find the original collection
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(),
        originalNss,
        targetNss,
        uuidWithDbName,
        renameCollectionTs,
        true,
        true,
        0,
        [&]() {
            // Verify that we can find the original Collection when we search by namespace as rename
            // has not committed yet.
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss);
            ASSERT(coll);
            ASSERT_EQ(coll->uuid(), originalUUID);

            // Verify that we can find the target Collection when we search by namespace as rename
            // has not committed yet.
            coll = CollectionCatalog::get(opCtx.get())
                       ->lookupCollectionByNamespace(opCtx.get(), targetNss);
            ASSERT(coll);
            ASSERT_EQ(coll->uuid(), targetUUID);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionWithTargetUUIDAfterCommit) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    // We expect to find the UUID for the original collection
    UUID originalUUID = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                            ->uuid();
    UUID targetUUID = CollectionCatalog::get(opCtx.get())
                          ->lookupCollectionByNamespace(opCtx.get(), targetNss)
                          ->uuid();
    NamespaceStringOrUUID uuidWithDbName(originalNss.dbName(), targetUUID);

    // When the snapshot is opened right after the rename is committed to the durable catalog, and
    // the openCollection looks for the newNss, no collection instance should be returned.
    concurrentRenameCollectionAndEstablishConsistentCollection(
        opCtx.get(),
        originalNss,
        targetNss,
        uuidWithDbName,
        renameCollectionTs,
        false,
        false,
        0,
        [&]() {
            // Verify that we can find the original Collection UUID when we search by namespace.
            auto coll = CollectionCatalog::get(opCtx.get())
                            ->lookupCollectionByNamespace(opCtx.get(), targetNss);
            ASSERT(coll);
            ASSERT_EQ(coll->uuid(), originalUUID);
        });
}

TEST_F(CollectionCatalogTimestampTest, UUIDLookupWhileCommitPendingRename) {
    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString newNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createCollectionTs);
    UUID uuid = CollectionCatalog::get(opCtx.get())
                    ->lookupCollectionByNamespace(opCtx.get(), originalNss)
                    ->uuid();

    concurrentRenameAndRunCatalogOperations(
        opCtx.get(),
        originalNss,
        newNss,
        renameCollectionTs,
        [this, &originalNss, &newNss, &uuid](OperationContext* opCtx) {
            auto catalog = CollectionCatalog::get(opCtx);
            ASSERT_EQ(catalog->resolveNamespaceStringOrUUID(opCtx, {originalNss.dbName(), uuid}),
                      originalNss);
            ASSERT_EQ(catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                          opCtx, {originalNss.dbName(), uuid}),
                      newNss);
        });
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    // When the snapshot is opened right before the second index create is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          nss,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          true,
                                                          true,
                                                          1);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateIndexAndOpenCollectionBeforeCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    // When the snapshot is opened right before the second index create is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          nss,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          true,
                                                          true,
                                                          1,
                                                          makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    // When the snapshot is opened right before the second index create is committed to the durable
    // catalog, the collection instance should have both indexes.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          nss,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          false,
                                                          true,
                                                          2);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateIndexAndOpenCollectionAfterCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    // When the snapshot is opened right after the second index create is committed to the durable
    // catalog, the collection instance should have both indexes.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          nss,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          false,
                                                          true,
                                                          2,
                                                          makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionByUUIDBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the second index create is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          uuidWithDbName,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          true,
                                                          true,
                                                          1);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateIndexAndOpenCollectionByUUIDBeforeCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the second index create is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          uuidWithDbName,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          true,
                                                          true,
                                                          1,
                                                          makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionByUUIDAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the second index create is committed to the durable
    // catalog, the collection instance should have both indexes.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          uuidWithDbName,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          false,
                                                          true,
                                                          2);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateIndexAndOpenCollectionByUUIDAfterCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createXIndexTs = Timestamp(20, 20);
    const Timestamp createYIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createXIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right after the second index create is committed to the durable
    // catalog, the collection instance should have both indexes.
    concurrentCreateIndexAndEstablishConsistentCollection(opCtx.get(),
                                                          nss,
                                                          uuidWithDbName,
                                                          BSON("v" << 2 << "name"
                                                                   << "y_1"
                                                                   << "key" << BSON("y" << 1)),
                                                          createYIndexTs,
                                                          false,
                                                          true,
                                                          2,
                                                          makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    // When the snapshot is opened right before the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, nss, "y_1", dropIndexTs, true, true, 2);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentDropIndexAndOpenCollectionBeforeCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    // When the snapshot is opened right before the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, nss, "y_1", dropIndexTs, true, true, 2, makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    // When the snapshot is opened right before the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, nss, "y_1", dropIndexTs, false, true, 1);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentDropIndexAndOpenCollectionAfterCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    // When the snapshot is opened right after the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, nss, "y_1", dropIndexTs, false, true, 1, makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionByUUIDBeforeCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, uuidWithDbName, "y_1", dropIndexTs, true, true, 2);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentDropIndexAndOpenCollectionByUUIDBeforeCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, uuidWithDbName, "y_1", dropIndexTs, true, true, 2, makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionByUUIDAfterCommit) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);
    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right before the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, uuidWithDbName, "y_1", dropIndexTs, false, true, 1);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentDropIndexAndOpenCollectionByUUIDAfterCommitWithUnrelatedMultikey) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropIndexTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "x_1"
                         << "key" << BSON("x" << 1)),
                createIndexTs);
    createIndex(opCtx.get(),
                nss,
                BSON("v" << 2 << "name"
                         << "y_1"
                         << "key" << BSON("y" << 1)),
                createIndexTs);

    auto makeIndexMultikey = [nss](OperationContext* opCtx) {
        auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        coll->setIndexIsMultikey(opCtx, "x_1", {{0U}});
    };

    UUID uuid =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss)->uuid();
    NamespaceStringOrUUID uuidWithDbName(nss.dbName(), uuid);

    // When the snapshot is opened right after the index drop is committed to the durable
    // catalog, the collection instance should not have the second index.
    concurrentDropIndexAndEstablishConsistentCollection(
        opCtx.get(), nss, uuidWithDbName, "y_1", dropIndexTs, false, true, 1, makeIndexMultikey);
}

TEST_F(CollectionCatalogTimestampTest, OpenCollectionBetweenIndexBuildInProgressAndReady) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp indexReadyTs = Timestamp(30, 30);

    createCollection(opCtx.get(), nss, createCollectionTs);

    auto indexBuildBlock = createIndexWithoutFinishingBuild(opCtx.get(),
                                                            nss,
                                                            BSON("v" << 2 << "name"
                                                                     << "x_1"
                                                                     << "key" << BSON("x" << 1)),
                                                            createIndexTs);

    // Confirm openCollection with timestamp createCollectionTs indicates no indexes.
    {
        OneOffRead oor(opCtx.get(), createCollectionTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        auto coll = CollectionCatalog::get(opCtx.get())
                        ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(coll);
        ASSERT_EQ(coll->getIndexCatalog()->numIndexesReady(), 0);

        // Lookups from the catalog should return the newly opened collection.
        ASSERT_EQ(CollectionCatalog::get(opCtx.get())
                      ->lookupCollectionByNamespace(opCtx.get(), coll->ns()),
                  coll.get());
        ASSERT_EQ(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), coll->uuid()),
            coll.get());
    }

    finishIndexBuild(opCtx.get(), nss, std::move(indexBuildBlock), indexReadyTs);

    // Confirm openCollection with timestamp createIndexTs returns the same value as before, once
    // the index build has finished (since it can no longer use the latest state).
    {
        OneOffRead oor(opCtx.get(), createIndexTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        auto coll = CollectionCatalog::get(opCtx.get())
                        ->establishConsistentCollection(opCtx.get(), nss, createIndexTs);
        ASSERT(coll);
        ASSERT_EQ(coll->getIndexCatalog()->numIndexesReady(), 0);

        // Lookups from the catalog should return the newly opened collection.
        ASSERT_EQ(CollectionCatalog::get(opCtx.get())
                      ->lookupCollectionByNamespace(opCtx.get(), coll->ns()),
                  coll.get());
        ASSERT_EQ(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), coll->uuid()),
            coll.get());
    }
}

TEST_F(CollectionCatalogTimestampTest, ResolveNamespaceStringOrUUIDAtLatest) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const UUID uuid = createCollection(opCtx.get(), nss, createCollectionTs);
    const NamespaceStringOrUUID nssOrUUID = NamespaceStringOrUUID(nss.dbName(), uuid);

    NamespaceString resolvedNss =
        CollectionCatalog::get(opCtx.get())->resolveNamespaceStringOrUUID(opCtx.get(), nssOrUUID);
    ASSERT_EQ(resolvedNss, nss);

    const Timestamp dropCollectionTs = Timestamp(20, 20);
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    // Resolving the UUID throws NamespaceNotFound as the collection is no longer in the latest
    // collection catalog.
    ASSERT_THROWS_CODE(
        CollectionCatalog::get(opCtx.get())->resolveNamespaceStringOrUUID(opCtx.get(), nssOrUUID),
        DBException,
        ErrorCodes::NamespaceNotFound);

    {
        OneOffRead oor(opCtx.get(), createCollectionTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs);

        // Resolving the UUID looks in OpenedCollections to try to resolve the UUID.
        resolvedNss = CollectionCatalog::get(opCtx.get())
                          ->resolveNamespaceStringOrUUID(opCtx.get(), nssOrUUID);
        ASSERT_EQ(resolvedNss, nss);
    }
}

TEST_F(CollectionCatalogTimestampTest, IndexCatalogEntryCopying) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test.abc");
    createCollection(opCtx.get(), nss, Timestamp::min());

    {
        // Start but do not finish an index build.
        IndexSpec spec;
        spec.version(1).name("x_1").addKeys(BSON("x" << 1));
        auto desc = IndexDescriptor(IndexNames::BTREE, spec.toBSON());
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
        WriteUnitOfWork wuow(opCtx.get());
        CollectionWriter writer{opCtx.get(), autoColl};
        auto writableColl = writer.getWritableCollection(opCtx.get());
        ASSERT_OK(
            writableColl->prepareForIndexBuild(opCtx.get(), &desc, "index-ident", boost::none));
        wuow.commit();
    }

    // In a different client, open the latest collection instance and verify the index is not ready.
    auto newClient = opCtx->getServiceContext()->getService()->makeClient("alternativeClient");
    auto newOpCtx = newClient->makeOperationContext();
    auto latestCatalog = CollectionCatalog::latest(newOpCtx.get());
    auto latestColl =
        latestCatalog->establishConsistentCollection(newOpCtx.get(), nss, boost::none);

    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesTotal());
    ASSERT_EQ(0, latestColl->getIndexCatalog()->numIndexesReady());
    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesInProgress());
    const IndexDescriptor* desc = latestColl->getIndexCatalog()->findIndexByName(
        newOpCtx.get(), "x_1", IndexCatalog::InclusionPolicy::kUnfinished);
    const IndexCatalogEntry* entry = latestColl->getIndexCatalog()->getEntry(desc);
    ASSERT(!entry->isReady());

    {
        // Now finish the index build on the original client.
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
        WriteUnitOfWork wuow(opCtx.get());
        CollectionWriter writer{opCtx.get(), autoColl};
        auto writableColl = writer.getWritableCollection(opCtx.get());
        auto writableEntry = writableColl->getIndexCatalog()->getWritableEntryByName(
            opCtx.get(), "x_1", IndexCatalog::InclusionPolicy::kUnfinished);
        ASSERT_NOT_EQUALS(desc, writableEntry->descriptor());
        writableColl->getIndexCatalog()->indexBuildSuccess(
            opCtx.get(), writableColl, writableEntry);
        ASSERT(writableEntry->isReady());
        wuow.commit();
    }

    // The index entry in the different client remains untouched.
    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesTotal());
    ASSERT_EQ(0, latestColl->getIndexCatalog()->numIndexesReady());
    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesInProgress());
    ASSERT(!entry->isReady());
}

TEST_F(CollectionCatalogTimestampTest, OpenCollectionAfterDropAndCreateWithSameNsAndUUID) {
    // Simulates a scenario where a collection was dropped and later recreated using the same
    // NS and UUID. This can be the case when moving collections to another replica set / shard and
    // then back to the original owner.
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    auto collUUID = UUID::gen();

    // Create and drop collection.
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp firstCollectionDropTs = Timestamp(20, 20);
    createCollectionWithUUID(opCtx.get(), nss, createCollectionTs, collUUID);

    const auto firstIdent = std::string{CollectionCatalog::get(opCtx.get())
                                            ->lookupCollectionByNamespace(opCtx.get(), nss)
                                            ->getRecordStore()
                                            ->getIdent()};

    dropCollection(opCtx.get(), nss, firstCollectionDropTs);

    // Recreate collection with same NS and UUID, at a later timestamp.
    const Timestamp createCollectionSecondTs = Timestamp(30, 30);
    createCollectionWithUUID(opCtx.get(), nss, createCollectionSecondTs, collUUID);

    auto latestCatalog = CollectionCatalog::get(opCtx.get());
    auto latestCollection = latestCatalog->lookupCollectionByNamespace(opCtx.get(), nss);
    const auto secondIdent = latestCollection->getRecordStore()->getIdent();

    // Open the collection at a point in-time before the drop.
    const Timestamp readTimestamp = Timestamp(15, 15);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    OneOffRead oor(opCtx.get(), readTimestamp);
    auto coll = CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);

    // The two collection idents are different, thus the RecordStore pointers must be different.
    ASSERT_NE(firstIdent, secondIdent);
    ASSERT_NE(coll->getRecordStore(), latestCollection->getRecordStore());

    // Open the PIT collection in another client.
    auto newClient = opCtx->getServiceContext()->getService()->makeClient("alternativeClient");
    auto newOpCtx = newClient->makeOperationContext();
    Lock::GlobalLock globalLockAlt(newOpCtx.get(), MODE_IS);
    OneOffRead oorAlt(newOpCtx.get(), readTimestamp);
    auto collAlt = CollectionCatalog::get(newOpCtx.get())
                       ->establishConsistentCollection(newOpCtx.get(), nss, readTimestamp);

    // Both PIT reads at the same timestamp should point to the same ident, but the RecordStore
    // pointer will be different. This is expected, as PIT created collections are local to the
    // operation. This is fine because the RecordStore is only ever used to read (it is not possible
    // to write in the past).
    ASSERT_EQ(coll->getRecordStore()->getIdent(), collAlt->getRecordStore()->getIdent());
    ASSERT_NE(coll->getRecordStore(), collAlt->getRecordStore());
}

TEST_F(CollectionCatalogTimestampTest, MixedModeWrites) {
    // This tests checks the following sequence: untimestamped collection create
    // -> timestamped drop -> untimestamped collection recreate.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Initialize the oldest timestamp.
    {
        Lock::GlobalLock lk{opCtx.get(), MODE_IX};
        CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
            catalog.catalogIdTracker().cleanup(Timestamp(1, 1));
        });
    }
    // Create and drop the collection. We have a time window where the namespace exists.
    createCollection(opCtx.get(), nss, Timestamp::min(), true /* allowMixedModeWrite */);
    dropCollection(opCtx.get(), nss, Timestamp(10, 10));

    // Before performing cleanup, re-create the collection.
    createCollection(opCtx.get(), nss, Timestamp::min(), true /* allowMixedModeWrite */);

    // Perform collection catalog cleanup.
    {
        Lock::GlobalLock lk{opCtx.get(), MODE_IX};
        CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
            catalog.catalogIdTracker().cleanup(Timestamp(20, 20));
        });
    }
    // Drop the re-created collection.
    dropCollection(opCtx.get(), nss, Timestamp(30, 30));

    // Cleanup again.
    {
        Lock::GlobalLock lk{opCtx.get(), MODE_IX};
        CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
            catalog.catalogIdTracker().cleanup(Timestamp(40, 40));
        });
    }
}

TEST(GetConfigDebugDumpTest, NonConfigDatabase) {
    // Run against a non-config database. It should always return boost::none.

    const auto resultNonConfig = catalog::getConfigDebugDump(
        kNoVersionContext, NamespaceString::createNamespaceString_forTest("nonConfig", "dummy"));
    ASSERT_FALSE(resultNonConfig);

    const auto resultNonConfigDatabases = catalog::getConfigDebugDump(
        kNoVersionContext,
        NamespaceString::createNamespaceString_forTest("nonConfig", "databases"));
    ASSERT_FALSE(resultNonConfigDatabases);
};

TEST(GetConfigDebugDumpTest, ConfigDatabase) {
    // Run against the config database. It should always return a result, and it should be true or
    // false depending on the collection.

    const auto resultNotListed = catalog::getConfigDebugDump(
        kNoVersionContext, NamespaceString::createNamespaceString_forTest("config", "dummy"));
    ASSERT_TRUE(resultNotListed);
    ASSERT_FALSE(*resultNotListed);

    const auto resultListed = catalog::getConfigDebugDump(
        kNoVersionContext, NamespaceString::createNamespaceString_forTest("config", "databases"));
    ASSERT_TRUE(resultListed);
    ASSERT_TRUE(*resultListed);
};

// TODO (SERVER-95599): remove once 9.0 becomes last LTS.
TEST(GetConfigDebugDumpTest, FeatureFlagDisabled) {
    // With the feature flag disabled, it should always return boost::none.

    RAIIServerParameterControllerForTest featureFlagScope("featureFlagConfigDebugDumpSupported",
                                                          false);

    const auto resultNonConfig = catalog::getConfigDebugDump(
        kNoVersionContext, NamespaceString::createNamespaceString_forTest("nonConfig", "dummy"));
    ASSERT_FALSE(resultNonConfig);

    const auto resultNonConfigDatabases = catalog::getConfigDebugDump(
        kNoVersionContext,
        NamespaceString::createNamespaceString_forTest("nonConfig", "databases"));
    ASSERT_FALSE(resultNonConfigDatabases);

    const auto resultNotListed = catalog::getConfigDebugDump(
        kNoVersionContext, NamespaceString::createNamespaceString_forTest("config", "dummy"));
    ASSERT_FALSE(resultNotListed);

    const auto resultListed = catalog::getConfigDebugDump(
        kNoVersionContext, NamespaceString::createNamespaceString_forTest("config", "databases"));
    ASSERT_FALSE(resultListed);
};

}  // namespace
}  // namespace mongo
