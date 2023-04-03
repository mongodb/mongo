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

#include "mongo/db/catalog/collection_catalog.h"

#include <algorithm>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/collection_yield_restore.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/resource_catalog.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * A test fixture that creates a CollectionCatalog and const CollectionPtr& pointer to store in it.
 */
class CollectionCatalogTest : public ServiceContextMongoDTest {
public:
    CollectionCatalogTest()
        : nss("testdb", "testcol"),
          col(nullptr),
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
        col = CollectionPtr(collection.get());
        // Register dummy collection in catalog.
        catalog.registerCollection(opCtx.get(), colUUID, collection, boost::none);

        // Validate that kNumCollectionReferencesStored is correct, add one reference for the one we
        // hold in this function.
        ASSERT_EQUALS(collection.use_count(),
                      CollectionCatalog::kNumCollectionReferencesStored + 1);
    }

    void tearDown() {
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
    void setUp() {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
        globalLock.emplace(opCtx.get());

        for (int counter = 0; counter < 5; ++counter) {
            NamespaceString fooNss = NamespaceString::createNamespaceString_forTest(
                "foo", "coll" + std::to_string(counter));
            NamespaceString barNss = NamespaceString::createNamespaceString_forTest(
                "bar", "coll" + std::to_string(counter));

            auto fooUuid = UUID::gen();
            std::shared_ptr<Collection> fooColl = std::make_shared<CollectionMock>(fooNss);

            auto barUuid = UUID::gen();
            std::shared_ptr<Collection> barColl = std::make_shared<CollectionMock>(barNss);

            dbMap["foo"].insert(std::make_pair(fooUuid, fooColl.get()));
            dbMap["bar"].insert(std::make_pair(barUuid, barColl.get()));

            catalog.registerCollection(opCtx.get(), fooUuid, fooColl, boost::none);
            catalog.registerCollection(opCtx.get(), barUuid, barColl, boost::none);
        }
    }

    void tearDown() {
        for (auto& it : dbMap) {
            for (auto& kv : it.second) {
                catalog.deregisterCollection(
                    opCtx.get(), kv.first, /*isDropPending=*/false, boost::none);
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

        for (auto [orderedIt, catalogIt] =
                 std::tuple{collsIterator(dbName.toString()), catalog.begin(opCtx.get(), dbName)};
             catalogIt != catalog.end(opCtx.get()) &&
             orderedIt != collsIteratorEnd(dbName.toString());
             ++catalogIt, ++orderedIt) {

            auto catalogColl = *catalogIt;
            ASSERT(catalogColl);
            const auto& orderedColl = orderedIt->second;
            ASSERT_EQ(catalogColl->ns(), orderedColl->ns());
            ++counter;
        }

        ASSERT_EQUALS(counter, dbMap[dbName.toString()].size());
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
    void setUp() {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
        globalLock.emplace(opCtx.get());

        for (int i = 0; i < 5; i++) {
            NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                "resourceDb", "coll" + std::to_string(i));
            std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
            auto uuid = collection->uuid();

            catalog.registerCollection(opCtx.get(), uuid, std::move(collection), boost::none);
        }

        int numEntries = 0;
        for (auto it = catalog.begin(opCtx.get(), DatabaseName(boost::none, "resourceDb"));
             it != catalog.end(opCtx.get());
             it++) {
            auto coll = *it;
            auto collName = coll->ns();
            ResourceId rid(RESOURCE_COLLECTION, collName);

            ASSERT_NE(ResourceCatalog::get(getServiceContext()).name(rid), boost::none);
            numEntries++;
        }
        ASSERT_EQ(5, numEntries);
    }

    void tearDown() {
        std::vector<UUID> collectionsToDeregister;
        for (auto it = catalog.begin(opCtx.get(), DatabaseName(boost::none, "resourceDb"));
             it != catalog.end(opCtx.get());
             ++it) {
            auto coll = *it;
            auto uuid = coll->uuid();
            if (!coll) {
                break;
            }

            collectionsToDeregister.push_back(uuid);
        }

        for (auto&& uuid : collectionsToDeregister) {
            catalog.deregisterCollection(opCtx.get(), uuid, /*isDropPending=*/false, boost::none);
        }

        int numEntries = 0;
        for (auto it = catalog.begin(opCtx.get(), DatabaseName(boost::none, "resourceDb"));
             it != catalog.end(opCtx.get());
             it++) {
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

    const DatabaseName dbName = DatabaseName(boost::none, "resourceDb");
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    ASSERT_EQ(boost::none, ResourceCatalog::get(getServiceContext()).name(rid));

    for (int i = 0; i < 5; i++) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            "resourceDb", "coll" + std::to_string(i));
        rid = ResourceId(RESOURCE_COLLECTION, nss);
        ASSERT_EQ(boost::none, ResourceCatalog::get(getServiceContext()).name(rid));
    }
}

TEST_F(CollectionCatalogResourceTest, LookupDatabaseResource) {
    const DatabaseName dbName = DatabaseName(boost::none, "resourceDb");
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    auto ridStr = ResourceCatalog::get(getServiceContext()).name(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(dbName.toStringWithTenantId()) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingDatabaseResource) {
    const DatabaseName dbName = DatabaseName(boost::none, "missingDb");
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    ASSERT(!ResourceCatalog::get(getServiceContext()).name(rid));
}

TEST_F(CollectionCatalogResourceTest, LookupCollectionResource) {
    const NamespaceString collNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "resourceDb.coll1");
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    auto ridStr = ResourceCatalog::get(getServiceContext()).name(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(collNs.toStringWithTenantId()) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingCollectionResource) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "resourceDb.coll5");
    auto rid = ResourceId(RESOURCE_COLLECTION, nss);
    ASSERT(!ResourceCatalog::get(getServiceContext()).name(rid));
}

TEST_F(CollectionCatalogResourceTest, RemoveCollection) {
    const NamespaceString collNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "resourceDb.coll1");
    auto coll = catalog.lookupCollectionByNamespace(opCtx.get(), NamespaceString(collNs));
    catalog.deregisterCollection(opCtx.get(), coll->uuid(), /*isDropPending=*/false, boost::none);
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    ASSERT(!ResourceCatalog::get(getServiceContext()).name(rid));
}

// Create an iterator over the CollectionCatalog and assert that all collections are present.
// Iteration ends when the end of the catalog is reached.
TEST_F(CollectionCatalogIterationTest, EndAtEndOfCatalog) {
    checkCollections(DatabaseName(boost::none, "foo"));
}

// Create an iterator over the CollectionCatalog and test that all collections are present.
// Iteration ends
// when the end of a database-specific section of the catalog is reached.
TEST_F(CollectionCatalogIterationTest, EndAtEndOfSection) {
    checkCollections(DatabaseName(boost::none, "bar"));
}

TEST_F(CollectionCatalogIterationTest, GetUUIDWontRepositionEvenIfEntryIsDropped) {
    auto it = catalog.begin(opCtx.get(), DatabaseName(boost::none, "bar"));
    auto collsIt = collsIterator("bar");
    auto uuid = collsIt->first;
    catalog.deregisterCollection(opCtx.get(), uuid, /*isDropPending=*/false, boost::none);
    dropColl("bar", uuid);

    ASSERT_EQUALS(uuid, it.uuid());
}

TEST_F(CollectionCatalogTest, OnCreateCollection) {
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == col.get());
}

TEST_F(CollectionCatalogTest, LookupCollectionByUUID) {
    // Ensure the string value of the NamespaceString of the obtained Collection is equal to
    // nss.ns().
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), colUUID)->ns().ns(), nss.ns());
    // Ensure lookups of unknown UUIDs result in null pointers.
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), UUID::gen()) == nullptr);
}

TEST_F(CollectionCatalogTest, LookupNSSByUUID) {
    // Ensure the string value of the obtained NamespaceString is equal to nss.ns().
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), colUUID)->ns(), nss.ns());
    // Ensure namespace lookups of unknown UUIDs result in empty NamespaceStrings.
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), UUID::gen()), boost::none);
}

TEST_F(CollectionCatalogTest, InsertAfterLookup) {
    auto newUUID = UUID::gen();
    NamespaceString newNss = NamespaceString::createNamespaceString_forTest(nss.dbName(), "newcol");
    std::shared_ptr<Collection> newCollShared = std::make_shared<CollectionMock>(newNss);
    auto newCol = newCollShared.get();

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), newUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), newUUID), boost::none);
    catalog.registerCollection(opCtx.get(), newUUID, std::move(newCollShared), boost::none);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), newUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);
}

TEST_F(CollectionCatalogTest, OnDropCollection) {
    CollectionPtr yieldableColl(catalog.lookupCollectionByUUID(opCtx.get(), colUUID));
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
    CollectionCatalogStasher catalogStasher(opCtx.get(), sharedCatalog);

    // Before dropping collection, confirm that the CollectionPtr can be restored successfully.
    yieldableColl.restore();
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, col);

    // Reset CollectionPtr for post-drop restore test.
    yieldableColl.yield();
    ASSERT_FALSE(yieldableColl);

    catalog.deregisterCollection(opCtx.get(), colUUID, /*isDropPending=*/false, boost::none);
    // Ensure the lookup returns a null pointer upon removing the colUUID entry.
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);

    // After dropping the collection, we should fail to restore the CollectionPtr.
    yieldableColl.restore();
    ASSERT_FALSE(yieldableColl);
}

TEST_F(CollectionCatalogTest, RenameCollection) {
    auto uuid = UUID::gen();
    NamespaceString oldNss = NamespaceString::createNamespaceString_forTest(nss.db(), "oldcol");
    std::shared_ptr<Collection> collShared = std::make_shared<CollectionMock>(uuid, oldNss);
    auto collection = collShared.get();
    catalog.registerCollection(opCtx.get(), uuid, std::move(collShared), boost::none);
    CollectionPtr yieldableColl(catalog.lookupCollectionByUUID(opCtx.get(), uuid));
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, CollectionPtr(collection));

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
    CollectionCatalogStasher catalogStasher(opCtx.get(), sharedCatalog);

    // Before renaming collection, confirm that the CollectionPtr can be restored successfully.
    yieldableColl.restore();
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, CollectionPtr(collection));

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

    catalog.deregisterCollection(opCtx.get(), colUUID, /*isDropPending=*/false, boost::none);
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
    std::shared_ptr<Collection> newCollShared = std::make_shared<CollectionMock>(newNss);
    auto newCol = newCollShared.get();

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog();
    }

    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), newUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), newUUID), boost::none);
    catalog.registerCollection(opCtx.get(), newUUID, std::move(newCollShared), boost::none);
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
    std::shared_ptr<Collection> newCollShared = std::make_shared<CollectionMock>(newNss);
    auto newCol = newCollShared.get();

    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog();
    }

    catalog.deregisterCollection(opCtx.get(), colUUID, /*isDropPending=*/false, boost::none);
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);
    {
        Lock::GlobalWrite lk(opCtx.get());
        catalog.registerCollection(opCtx.get(), colUUID, std::move(newCollShared), boost::none);
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
        auto uuid = UUID::gen();
        catalog.registerCollection(opCtx.get(), uuid, std::move(newColl), boost::none);
    }

    std::vector<NamespaceString> dCollList = {d1Coll, d2Coll, d3Coll};

    Lock::DBLock dbLock(opCtx.get(), d1Coll.dbName(), MODE_S);
    auto res = catalog.getAllCollectionNamesFromDb(opCtx.get(), d1Coll.dbName());
    std::sort(res.begin(), res.end());
    ASSERT(res == dCollList);

    std::vector<DatabaseName> dbNames = {DatabaseName(boost::none, "dbA"),
                                         DatabaseName(boost::none, "dbB"),
                                         DatabaseName(boost::none, "dbC"),
                                         DatabaseName(boost::none, "dbD"),
                                         DatabaseName(boost::none, "testdb")};
    ASSERT(catalog.getAllDbNames() == dbNames);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

TEST_F(CollectionCatalogTest, GetAllDbNamesForTenant) {
    TenantId tid1 = TenantId(OID::gen());
    TenantId tid2 = TenantId(OID::gen());
    NamespaceString dbA = NamespaceString::createNamespaceString_forTest(tid1, "dbA.collA");
    NamespaceString dbB = NamespaceString::createNamespaceString_forTest(tid1, "dbB.collA");
    NamespaceString dbC = NamespaceString::createNamespaceString_forTest(tid1, "dbC.collA");
    NamespaceString dbD = NamespaceString::createNamespaceString_forTest(tid2, "dbB.collA");

    std::vector<NamespaceString> nsss = {dbA, dbB, dbC, dbD};
    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        auto uuid = UUID::gen();
        catalog.registerCollection(opCtx.get(), uuid, std::move(newColl), boost::none);
    }

    std::vector<DatabaseName> dbNamesForTid1 = {
        DatabaseName(tid1, "dbA"), DatabaseName(tid1, "dbB"), DatabaseName(tid1, "dbC")};
    ASSERT(catalog.getAllDbNamesForTenant(tid1) == dbNamesForTid1);

    std::vector<DatabaseName> dbNamesForTid2 = {DatabaseName(tid2, "dbB")};
    ASSERT(catalog.getAllDbNamesForTenant(tid2) == dbNamesForTid2);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
}

// Test setting and fetching the profile level for a database.
TEST_F(CollectionCatalogTest, DatabaseProfileLevel) {
    DatabaseName testDBNameFirst(boost::none, "testdbfirst");
    DatabaseName testDBNameSecond(boost::none, "testdbsecond");

    // Requesting a profile level that is not in the _databaseProfileLevel map should return the
    // default server-wide setting
    ASSERT_EQ(catalog.getDatabaseProfileSettings(testDBNameFirst).level,
              serverGlobalParams.defaultProfile);
    // Setting the default profile level should have not change the result.
    catalog.setDatabaseProfileSettings(testDBNameFirst,
                                       {serverGlobalParams.defaultProfile, nullptr});
    ASSERT_EQ(catalog.getDatabaseProfileSettings(testDBNameFirst).level,
              serverGlobalParams.defaultProfile);

    // Changing the profile level should make fetching it different.
    catalog.setDatabaseProfileSettings(testDBNameSecond,
                                       {serverGlobalParams.defaultProfile + 1, nullptr});
    ASSERT_EQ(catalog.getDatabaseProfileSettings(testDBNameSecond).level,
              serverGlobalParams.defaultProfile + 1);
}

TEST_F(CollectionCatalogTest, GetAllCollectionNamesAndGetAllDbNamesWithUncommittedCollections) {
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
        auto uuid = UUID::gen();
        catalog.registerCollection(opCtx.get(), uuid, std::move(newColl), boost::none);
    }

    // One dbName with only an invisible collection does not appear in dbNames. Use const_cast to
    // modify the collection in the catalog inplace, this bypasses copy-on-write behavior.
    auto invisibleCollA =
        const_cast<Collection*>(catalog.lookupCollectionByNamespace(opCtx.get(), aColl));
    invisibleCollA->setCommitted(false);

    Lock::DBLock dbLock(opCtx.get(), aColl.dbName(), MODE_S);
    auto res = catalog.getAllCollectionNamesFromDb(opCtx.get(), DatabaseName(boost::none, "dbA"));
    ASSERT(res.empty());

    std::vector<DatabaseName> dbNames = {DatabaseName(boost::none, "dbB"),
                                         DatabaseName(boost::none, "dbC"),
                                         DatabaseName(boost::none, "dbD"),
                                         DatabaseName(boost::none, "testdb")};
    ASSERT(catalog.getAllDbNames() == dbNames);

    // One dbName with both visible and invisible collections is still visible.
    std::vector<NamespaceString> dbDNss = {d1Coll, d2Coll, d3Coll};
    for (auto& nss : dbDNss) {
        // Test each combination of one collection in dbD being invisible while the other two are
        // visible.
        std::vector<NamespaceString> dCollList = dbDNss;
        dCollList.erase(std::find(dCollList.begin(), dCollList.end(), nss));

        // Use const_cast to modify the collection in the catalog inplace, this bypasses
        // copy-on-write behavior.
        auto invisibleCollD =
            const_cast<Collection*>(catalog.lookupCollectionByNamespace(opCtx.get(), nss));
        invisibleCollD->setCommitted(false);

        Lock::DBLock dbLock(opCtx.get(), d1Coll.dbName(), MODE_S);
        res = catalog.getAllCollectionNamesFromDb(opCtx.get(), DatabaseName(boost::none, "dbD"));
        std::sort(res.begin(), res.end());
        ASSERT(res == dCollList);

        ASSERT(catalog.getAllDbNames() == dbNames);
        invisibleCollD->setCommitted(true);
    }

    invisibleCollA->setCommitted(true);  // reset visibility.

    // If all dbNames consist only of invisible collections, none of these dbs is visible.
    for (auto& nss : nsss) {
        // Use const_cast to modify the collection in the catalog inplace, this bypasses
        // copy-on-write behavior.
        auto invisibleColl =
            const_cast<Collection*>(catalog.lookupCollectionByNamespace(opCtx.get(), nss));
        invisibleColl->setCommitted(false);
    }

    std::vector<DatabaseName> dbList = {DatabaseName(boost::none, "testdb")};
    ASSERT(catalog.getAllDbNames() == dbList);

    catalog.deregisterAllCollectionsAndViews(getServiceContext());
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
        const DatabaseName dbName(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(opCtx, dbName, MODE_X, [&](const Collection* collection) {
            ASSERT_TRUE(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
            numCollectionsTraversed++;
            return true;
        });

        ASSERT_EQUALS(numCollectionsTraversed, 3);
    }

    {
        const DatabaseName dbName(boost::none, "db2");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(opCtx, dbName, MODE_IS, [&](const Collection* collection) {
            ASSERT_TRUE(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IS));
            numCollectionsTraversed++;
            return true;
        });

        ASSERT_EQUALS(numCollectionsTraversed, 1);
    }

    {
        const DatabaseName dbName(boost::none, "db3");
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
        const DatabaseName dbName(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx,
            dbName,
            MODE_X,
            [&](const Collection* collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
                numCollectionsTraversed++;
                return true;
            },
            [&](const Collection* collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_NONE));
                return collection->getCollectionOptions().temp;
            });

        ASSERT_EQUALS(numCollectionsTraversed, 2);
    }

    {
        const DatabaseName dbName(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx,
            dbName,
            MODE_IX,
            [&](const Collection* collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IX));
                numCollectionsTraversed++;
                return true;
            },
            [&](const Collection* collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_NONE));
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
        _opCtx->recoveryUnit()->abandonSnapshot();
        if (ts.isNull()) {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
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
                          Timestamp timestamp) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        UUID uuid = _createCollection(opCtx, nss);
        wuow.commit();
        return uuid;
    }

    CollectionCatalog::CatalogIdLookup lookupCatalogId(const NamespaceString& nss,
                                                       const UUID& uuid,
                                                       boost::optional<Timestamp> ts) {
        // Verify that lookups and NSS and UUID yield the same result.
        CollectionCatalog::CatalogIdLookup nssLookup = catalog()->lookupCatalogIdByNSS(nss, ts);
        CollectionCatalog::CatalogIdLookup uuidLookup = catalog()->lookupCatalogIdByUUID(uuid, ts);

        ASSERT_EQ(nssLookup.result, uuidLookup.result);
        ASSERT_EQ(nssLookup.id, uuidLookup.id);

        return nssLookup;
    }

    void dropCollection(OperationContext* opCtx, const NamespaceString& nss, Timestamp timestamp) {
        _setupDDLOperation(opCtx, timestamp);
        WriteUnitOfWork wuow(opCtx);
        _dropCollection(opCtx, nss, timestamp);
        wuow.commit();
    }

    void renameCollection(OperationContext* opCtx,
                          const NamespaceString& from,
                          const NamespaceString& to,
                          Timestamp timestamp) {
        invariant(from.db() == to.db());

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

        StatusWith<BSONObj> statusWithSpec = writableColl->getIndexCatalog()->prepareSpecForCreate(
            opCtx, CollectionPtr(writableColl), indexSpec, boost::none);
        uassertStatusOK(statusWithSpec.getStatus());
        indexSpec = statusWithSpec.getValue();

        auto indexBuildBlock = std::make_unique<IndexBuildBlock>(
            writableColl->ns(), indexSpec, IndexBuildMethod::kForeground, UUID::gen());
        uassertStatusOK(indexBuildBlock->init(opCtx, writableColl, /*forRecover=*/false));
        uassertStatusOK(indexBuildBlock->getEntry(opCtx, writableColl)
                            ->accessMethod()
                            ->initializeAsEmpty(opCtx));
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

    void concurrentCreateIndexAndEstablishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const NamespaceStringOrUUID& readNssOrUUID,
        BSONObj indexSpec,
        Timestamp timestamp,
        bool openSnapshotBeforeCommit,
        bool expectedExistence,
        int expectedNumIndexes) {
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            readNssOrUUID,
            timestamp,
            [this, &nss, &indexSpec](OperationContext* opCtx) {
                _createIndex(opCtx, nss, indexSpec);
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
        int expectedNumIndexes) {
        _concurrentDDLOperationAndEstablishConsistentCollection(
            opCtx,
            readNssOrUUID,
            timestamp,
            [this, &nss, &indexName](OperationContext* opCtx) {
                _dropIndex(opCtx, nss, indexName);
            },
            openSnapshotBeforeCommit,
            expectedExistence,
            expectedNumIndexes);
    }

protected:
    ServiceContext::UniqueOperationContext opCtx;

private:
    void _setupDDLOperation(OperationContext* opCtx, Timestamp timestamp) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        opCtx->recoveryUnit()->abandonSnapshot();

        if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
        opCtx->recoveryUnit()->setCommitTimestamp(timestamp);
    }

    UUID _createCollection(OperationContext* opCtx,
                           const NamespaceString& nss,
                           boost::optional<UUID> uuid = boost::none) {
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
        std::pair<RecordId, std::unique_ptr<RecordStore>> catalogIdRecordStorePair =
            uassertStatusOK(storageEngine->getCatalog()->createCollection(
                opCtx, nss, options, /*allocateDefaultSpace=*/true));
        auto& catalogId = catalogIdRecordStorePair.first;
        std::shared_ptr<Collection> ownedCollection = Collection::Factory::get(opCtx)->make(
            opCtx, nss, catalogId, options, std::move(catalogIdRecordStorePair.second));
        ownedCollection->init(opCtx);
        ownedCollection->setCommitted(false);

        // Adds the collection to the in-memory catalog.
        CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, std::move(ownedCollection));
        return *options.uuid;
    }

    void _dropCollection(OperationContext* opCtx, const NamespaceString& nss, Timestamp timestamp) {
        Lock::DBLock dbLk(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLk(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);

        Collection* writableCollection = collection.getWritableCollection(opCtx);

        // Drop all remaining indexes before dropping the collection.
        std::vector<std::string> indexNames;
        writableCollection->getAllIndexes(&indexNames);
        for (const auto& indexName : indexNames) {
            IndexCatalog* indexCatalog = writableCollection->getIndexCatalog();
            auto indexDescriptor = indexCatalog->findIndexByName(
                opCtx, indexName, IndexCatalog::InclusionPolicy::kReady);

            // This also adds the index ident to the drop-pending reaper.
            ASSERT_OK(indexCatalog->dropIndex(opCtx, writableCollection, indexDescriptor));
        }

        // Add the collection ident to the drop-pending reaper.
        opCtx->getServiceContext()->getStorageEngine()->addDropPendingIdent(
            timestamp, collection->getRecordStore()->getSharedIdent());

        // Drops the collection from the durable catalog.
        auto storageEngine = getServiceContext()->getStorageEngine();
        uassertStatusOK(
            storageEngine->getCatalog()->dropCollection(opCtx, writableCollection->getCatalogId()));

        // Drops the collection from the in-memory catalog.
        CollectionCatalog::get(opCtx)->dropCollection(
            opCtx, writableCollection, /*isDropPending=*/true);
    }

    void _renameCollection(OperationContext* opCtx,
                           const NamespaceString& from,
                           const NamespaceString& to,
                           Timestamp timestamp) {
        Lock::DBLock dbLk(opCtx, from.db(), MODE_IX);
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
        IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
            opCtx, collection, {indexSpec}, /*fromMigrate=*/false);
    }

    void _dropIndex(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const std::string& indexName) {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);

        CollectionWriter collection(opCtx, nss);

        Collection* writableCollection = collection.getWritableCollection(opCtx);

        IndexCatalog* indexCatalog = writableCollection->getIndexCatalog();
        auto indexDescriptor =
            indexCatalog->findIndexByName(opCtx, indexName, IndexCatalog::InclusionPolicy::kReady);

        // This also adds the index ident to the drop-pending reaper.
        ASSERT_OK(indexCatalog->dropIndex(opCtx, writableCollection, indexDescriptor));
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
        mongo::Mutex mutex;
        stdx::condition_variable cv;
        int numCalls = 0;

        stdx::thread t([&, svcCtx = getServiceContext()] {
            ThreadClient client(svcCtx);
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
                newOpCtx.get()->recoveryUnit()->onCommit(
                    [&commitHandler](OperationContext*, boost::optional<Timestamp>) {
                        commitHandler();
                    });
            }

            ddlOperation(newOpCtx.get());

            // The preCommit handler must be registered after the DDL operation so it's executed
            // after any preCommit hooks set up in the operation.
            if (openSnapshotBeforeCommit) {
                newOpCtx.get()->recoveryUnit()->registerPreCommitHook(
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
        const Collection* coll = CollectionCatalog::get(opCtx)->establishConsistentCollection(
            opCtx, nssOrUUID, boost::none);

        // Notify the thread that our openCollection lookup is done.
        {
            stdx::unique_lock lock(mutex);
            numCalls = 2;
            cv.notify_all();
        }
        t.join();


        auto catalog = CollectionCatalog::get(opCtx);
        if (expectedExistence) {
            ASSERT(coll);

            NamespaceString nss = catalog->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);

            ASSERT_EQ(coll->ns(), nss);
            // Check that lookup returns the same instance as openCollection above
            ASSERT_EQ(catalog->lookupCollectionByNamespace(opCtx, coll->ns()), coll);
            ASSERT_EQ(catalog->lookupCollectionByUUID(opCtx, coll->uuid()), coll);
            ASSERT_EQ(catalog->lookupNSSByUUID(opCtx, coll->uuid()), nss);
            ASSERT_EQ(coll->getIndexCatalog()->numIndexesTotal(), expectedNumIndexes);

            auto catalogEntry =
                DurableCatalog::get(opCtx)->getCatalogEntry(opCtx, coll->getCatalogId());
            ASSERT(!catalogEntry.isEmpty());
            ASSERT(
                coll->isMetadataEqual(DurableCatalog::getMetadataFromCatalogEntry(catalogEntry)));

            // Lookups from the catalog should return the newly opened collection.
            ASSERT_EQ(catalog->lookupCollectionByNamespace(opCtx, coll->ns()), coll);
            ASSERT_EQ(catalog->lookupCollectionByUUID(opCtx, coll->uuid()), coll);
        } else {
            ASSERT(!coll);
            if (auto nss = nssOrUUID.nss()) {
                auto catalogEntry =
                    DurableCatalog::get(opCtx)->scanForCatalogEntryByNss(opCtx, *nss);
                ASSERT(!catalogEntry);

                // Lookups from the catalog should return the newly opened collection (in this case
                // nullptr).
                ASSERT_EQ(catalog->lookupCollectionByNamespace(opCtx, *nss), coll);
            } else if (auto uuid = nssOrUUID.uuid()) {
                auto catalogEntry =
                    DurableCatalog::get(opCtx)->scanForCatalogEntryByUUID(opCtx, *uuid);
                ASSERT(!catalogEntry);

                // Lookups from the catalog should return the newly opened collection (in this case
                // nullptr).
                ASSERT_EQ(catalog->lookupCollectionByUUID(opCtx, *uuid), coll);
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
    ASSERT_EQ(coll->getMinimumVisibleSnapshot(), createCollectionTs);
    ASSERT_EQ(coll->getMinimumValidSnapshot(), createYIndexTs);

    const IndexDescriptor* desc = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    const IndexCatalogEntry* entry = coll->getIndexCatalog()->getEntry(desc);
    ASSERT_EQ(entry->getMinimumVisibleSnapshot(), createXIndexTs);

    desc = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "y_1");
    entry = coll->getIndexCatalog()->getEntry(desc);
    ASSERT_EQ(entry->getMinimumVisibleSnapshot(), createYIndexTs);

    dropIndex(opCtx.get(), nss, "x_1", dropIndexTs);
    dropIndex(opCtx.get(), nss, "y_1", dropIndexTs);

    // Fetch the latest collection instance without the indexes.
    coll = CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), nss);
    ASSERT(coll);
    ASSERT_EQ(coll->getMinimumVisibleSnapshot(), createCollectionTs);
    ASSERT_EQ(coll->getMinimumValidSnapshot(), dropIndexTs);
}

TEST_F(CollectionCatalogTimestampTest, OpenCollectionBeforeCreateTimestamp) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
              coll);
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);
    ASSERT(latestColl);
    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesTotal());

    // Ensure the idents are shared between the collection instances.
    ASSERT_NE(coll, latestColl);
    ASSERT_EQ(coll->getSharedIdent(), latestColl->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollectionWithIndex) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);
    ASSERT(latestColl);
    ASSERT_EQ(2, latestColl->getIndexCatalog()->numIndexesTotal());

    // Ensure the idents are shared between the collection and index instances.
    ASSERT_NE(coll, latestColl);
    ASSERT_EQ(coll->getSharedIdent(), latestColl->getSharedIdent());

    auto indexDescPast = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescLatest = latestColl->getIndexCatalog()->findIndexByName(newOpCtx.get(), "x_1");
    ASSERT_BSONOBJ_EQ(indexDescPast->infoObj(), indexDescLatest->infoObj());
    ASSERT_EQ(coll->getIndexCatalog()->getEntryShared(indexDescPast)->getSharedIdent(),
              latestColl->getIndexCatalog()->getEntryShared(indexDescLatest)->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenLatestCollectionWithIndex) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    ASSERT_EQ(coll, currentColl);

    // Ensure the idents are shared between the collection and index instances.
    ASSERT_EQ(coll->getSharedIdent(), currentColl->getSharedIdent());

    auto indexDesc = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescCurrent = currentColl->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    ASSERT_BSONOBJ_EQ(indexDesc->infoObj(), indexDescCurrent->infoObj());
    ASSERT_EQ(coll->getIndexCatalog()->getEntryShared(indexDesc)->getSharedIdent(),
              currentColl->getIndexCatalog()->getEntryShared(indexDescCurrent)->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollectionWithDropPendingIndex) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);
    ASSERT_NE(coll, latestColl);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl =
        CollectionCatalog::get(newOpCtx.get())->lookupCollectionByNamespace(newOpCtx.get(), nss);

    ASSERT_NE(coll, latestColl);

    auto indexDescZ = coll->getIndexCatalog()->findIndexByName(opCtx.get(), zIndexName);
    auto indexEntryZ = coll->getIndexCatalog()->getEntryShared(indexDescZ);
    auto indexEntryZIsMultikey = indexEntryZ->isMultikey(newOpCtx.get(), CollectionPtr(coll));

    ASSERT_FALSE(indexEntryZIsMultikey);
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierAlreadyDropPendingCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString firstNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString secondNss = NamespaceString::createNamespaceString_forTest("c.d");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(30, 30);

    createCollection(opCtx.get(), firstNss, createCollectionTs);
    createCollection(opCtx.get(), secondNss, createCollectionTs);

    // Maintain a shared_ptr to the catalog so that collection "a.b" isn't expired in the drop
    // pending map after we drop the collections.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto coll =

        catalog->lookupCollectionByNamespace(opCtx.get(), firstNss);
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
            openedColl);
        opCtx->recoveryUnit()->abandonSnapshot();

        // Once snapshot is abandoned, openedColl has been released so it should not match the
        // collection lookup.
        ASSERT_NE(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespace(opCtx.get(), firstNss),
            openedColl);
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
                  openedColl);
        opCtx->recoveryUnit()->abandonSnapshot();
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionUsingDropPendingCollectionSharedState) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    ASSERT_NE(coll, openedColl);
    // Ensure the idents are shared between the opened collection and the drop pending collection.
    ASSERT_EQ(coll->getSharedIdent(), openedColl->getSharedIdent());
    opCtx->recoveryUnit()->abandonSnapshot();
}

TEST_F(CollectionCatalogTimestampTest, OpenExistingCollectionWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Maintain a shared_ptr to the catalog so that the reaper cannot drop the collection ident.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto coll = catalog->lookupCollectionByNamespace(opCtx.get(), nss);
    ASSERT(coll);

    // Mark the collection as drop pending. The dropToken in the ident reaper is not expired as we
    // still have a reference.
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    {
        ASSERT_EQ(1, storageEngine->getNumDropPendingIdents());
        ASSERT_EQ(coll->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());

        // Ident is not expired and should not be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());

        ASSERT_EQ(1, storageEngine->getNumDropPendingIdents());
        ASSERT_EQ(coll->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());
    }

    {
        OneOffRead oor(opCtx.get(), createCollectionTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);
        ASSERT_EQ(coll->getSharedIdent(), openedColl->getSharedIdent());

        // The ident is now expired and should be removed the next time the ident reaper runs.
        catalog.reset();
    }

    {
        // Remove the collection reference in UncommittedCatalogUpdates.
        opCtx->recoveryUnit()->abandonSnapshot();

        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(0, storageEngine->getNumDropPendingIdents());

        // Now we fail to open the collection as the ident has been removed.
        OneOffRead oor(opCtx.get(), createCollectionTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Make the collection drop pending. The dropToken in the ident reaper is now expired as we
    // don't maintain any references to the collection.
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    {
        // Open the collection, which marks the ident as in use before running the ident reaper.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);

        ASSERT_EQ(1, storageEngine->getNumDropPendingIdents());
        ASSERT_EQ(openedColl->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());

        // Ident is marked as in use and it should not be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());

        ASSERT_EQ(1, storageEngine->getNumDropPendingIdents());
        ASSERT_EQ(openedColl->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());
    }

    {
        // Run the ident reaper before opening the collection.
        ASSERT_EQ(1, storageEngine->getNumDropPendingIdents());

        // The dropToken is expired as the ident is no longer in use.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());

        ASSERT_EQ(0, storageEngine->getNumDropPendingIdents());

        // Now we fail to open the collection as the ident has been removed.
        OneOffRead oor(opCtx.get(), createCollectionTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenExistingCollectionAndIndexesWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropXIndexTs = Timestamp(30, 30);
    const Timestamp dropYIndexTs = Timestamp(40, 40);
    const Timestamp dropCollectionTs = Timestamp(50, 50);

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

    // Perform index drops at different timestamps. By not maintaining shared_ptrs to the these
    // indexes, their idents are expired.
    dropIndex(opCtx.get(), nss, "x_1", dropXIndexTs);
    dropIndex(opCtx.get(), nss, "y_1", dropYIndexTs);

    // Maintain a shared_ptr to the catalog so that the reaper cannot drop the collection ident.
    auto catalog = CollectionCatalog::get(opCtx.get());
    auto coll = catalog->lookupCollectionByNamespace(opCtx.get(), nss);
    ASSERT(coll);

    dropCollection(opCtx.get(), nss, dropCollectionTs);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    ASSERT_EQ(3, storageEngine->getNumDropPendingIdents());

    {
        // Open the collection using shared state before any index drops.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, createIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(openedColl->getSharedIdent(), coll->getSharedIdent());
        ASSERT_EQ(2, openedColl->getIndexCatalog()->numIndexesTotal());

        // All idents are marked as in use and none should be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(3, storageEngine->getNumDropPendingIdents());
    }

    {
        // Open the collection using shared state after a single index was dropped.
        OneOffRead oor(opCtx.get(), dropXIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, dropXIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(openedColl->getSharedIdent(), coll->getSharedIdent());
        ASSERT_EQ(1, openedColl->getIndexCatalog()->numIndexesTotal());

        std::vector<std::string> indexNames;
        openedColl->getAllIndexes(&indexNames);
        ASSERT_EQ(1, indexNames.size());
        ASSERT_EQ("y_1", indexNames.front());

        // Only the collection and 'y' index idents are marked as in use. The 'x' index ident will
        // be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(2, storageEngine->getNumDropPendingIdents());
    }

    {
        // Open the collection using shared state before any indexes were created.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);
        ASSERT_EQ(openedColl->getSharedIdent(), coll->getSharedIdent());
        ASSERT_EQ(0, openedColl->getIndexCatalog()->numIndexesTotal());
    }

    {
        // Try to open the collection using shared state when both indexes were present. This should
        // fail as the ident for index 'x' was already removed.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, createIndexTs));

        ASSERT_EQ(2, storageEngine->getNumDropPendingIdents());
    }

    {
        // Drop all remaining idents.
        catalog.reset();

        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(0, storageEngine->getNumDropPendingIdents());

        // All idents are removed so opening the collection before any indexes were created should
        // fail.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionAndIndexesWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp createIndexTs = Timestamp(20, 20);
    const Timestamp dropXIndexTs = Timestamp(30, 30);
    const Timestamp dropYIndexTs = Timestamp(40, 40);
    const Timestamp dropCollectionTs = Timestamp(50, 50);

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

    // Perform drops at different timestamps. By not maintaining shared_ptrs to the these, their
    // idents are expired.
    dropIndex(opCtx.get(), nss, "x_1", dropXIndexTs);
    dropIndex(opCtx.get(), nss, "y_1", dropYIndexTs);
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    ASSERT_EQ(3, storageEngine->getNumDropPendingIdents());

    {
        // Open the collection before any index drops.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, createIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(2, openedColl->getIndexCatalog()->numIndexesTotal());

        // All idents are marked as in use and none should be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(3, storageEngine->getNumDropPendingIdents());
    }

    {
        // Open the collection after the 'x' index was dropped.
        OneOffRead oor(opCtx.get(), dropXIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, dropXIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(1, openedColl->getIndexCatalog()->numIndexesTotal());

        std::vector<std::string> indexNames;
        openedColl->getAllIndexes(&indexNames);
        ASSERT_EQ(1, indexNames.size());
        ASSERT_EQ("y_1", indexNames.front());

        // The 'x' index ident will be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(2, storageEngine->getNumDropPendingIdents());
    }

    {
        // Open the collection before any indexes were created.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto openedColl = CollectionCatalog::get(opCtx.get())
                              ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);
        ASSERT_EQ(0, openedColl->getIndexCatalog()->numIndexesTotal());
    }

    {
        // Try to open the collection before any index drops. Because the 'x' index ident is already
        // dropped, this should fail.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, createIndexTs));

        ASSERT_EQ(2, storageEngine->getNumDropPendingIdents());
    }

    {
        // Drop all remaining idents and try to open the collection. This should fail.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(0, storageEngine->getNumDropPendingIdents());

        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->establishConsistentCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCreate) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Initialize the oldest timestamp to (1, 1)
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 1));
    });

    // Create collection and extract the catalogId
    UUID uuid = createCollection(opCtx.get(), nss, Timestamp(1, 2));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();

    // Lookup without timestamp returns latest catalogId
    ASSERT_EQ(lookupCatalogId(nss, uuid, boost::none).id, rid);
    // Lookup before create returns unknown if looking before oldest
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 0)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    // Lookup before create returns not exists if looking after oldest
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 1)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup at create returns catalogId
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 2)).id, rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 3)).id, rid);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingDrop) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Initialize the oldest timestamp to (1, 1)
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 1));
    });

    // Create and drop collection. We have a time window where the namespace exists
    UUID uuid = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));

    // Lookup without timestamp returns none
    ASSERT_EQ(lookupCatalogId(nss, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup before create and oldest returns unknown
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 0)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    // Lookup before create returns not exists
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 4)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup at create returns catalogId
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 5)).id, rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 6)).id, rid);
    // Lookup at drop returns none
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 10)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup after drop returns none
    ASSERT_EQ(lookupCatalogId(nss, uuid, Timestamp(1, 20)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingRename) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString from = NamespaceString::createNamespaceString_forTest("a.b");
    NamespaceString to = NamespaceString::createNamespaceString_forTest("a.c");

    // Initialize the oldest timestamp to (1, 1)
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 1));
    });

    // Create and rename collection. We have two windows where the collection exists but for
    // different namespaces
    UUID uuid = createCollection(opCtx.get(), from, Timestamp(1, 5));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), from)->getCatalogId();
    renameCollection(opCtx.get(), from, to, Timestamp(1, 10));

    // Lookup without timestamp on 'from' returns none. By 'uuid' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(uuid, boost::none).id, rid);
    // Lookup before create and oldest returns unknown
    ASSERT_EQ(lookupCatalogId(from, uuid, Timestamp(1, 0)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    // Lookup before create returns not exists
    ASSERT_EQ(lookupCatalogId(from, uuid, Timestamp(1, 4)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup at create returns catalogId
    ASSERT_EQ(lookupCatalogId(from, uuid, Timestamp(1, 5)).id, rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(lookupCatalogId(from, uuid, Timestamp(1, 6)).id, rid);
    // Lookup at rename on 'from' returns none. By 'uuid' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 10)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(uuid, Timestamp(1, 10)).id, rid);
    // Lookup after rename on 'from' returns none. By 'uuid' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 20)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(uuid, Timestamp(1, 20)).id, rid);

    // Lookup without timestamp on 'to' returns catalogId
    ASSERT_EQ(lookupCatalogId(to, uuid, boost::none).id, rid);
    // Lookup before rename and oldest on 'to' returns unknown
    ASSERT_EQ(lookupCatalogId(to, uuid, Timestamp(1, 0)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    // Lookup before rename on 'to' returns not exists. By 'uuid' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(to, Timestamp(1, 9)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(uuid, Timestamp(1, 9)).id, rid);
    // Lookup at rename on 'to' returns catalogId
    ASSERT_EQ(lookupCatalogId(to, uuid, Timestamp(1, 10)).id, rid);
    // Lookup after rename on 'to' returns catalogId
    ASSERT_EQ(lookupCatalogId(to, uuid, Timestamp(1, 20)).id, rid);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingRenameDropTarget) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString from = NamespaceString::createNamespaceString_forTest("a.b");
    NamespaceString to = NamespaceString::createNamespaceString_forTest("a.c");

    // Initialize the oldest timestamp to (1, 1)
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 1));
    });

    // Create collections. The 'to' namespace will exist for one collection from Timestamp(1, 6)
    // until it is dropped by the rename at Timestamp(1, 10), after which the 'to' namespace will
    // correspond to the renamed collection.
    UUID uuid = createCollection(opCtx.get(), from, Timestamp(1, 5));
    UUID originalUUID = createCollection(opCtx.get(), to, Timestamp(1, 6));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), from)->getCatalogId();
    RecordId originalToRid =
        catalog()->lookupCollectionByNamespace(opCtx.get(), to)->getCatalogId();
    renameCollection(opCtx.get(), from, to, Timestamp(1, 10));

    // Lookup without timestamp on 'to' and 'uuid' returns latest catalog id. By 'originalUUID'
    // returns not exists as the target was dropped.
    ASSERT_EQ(lookupCatalogId(to, uuid, boost::none).id, rid);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(originalUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup before rename and oldest on 'to' returns unknown
    ASSERT_EQ(lookupCatalogId(to, uuid, Timestamp(1, 0)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(to, originalUUID, Timestamp(1, 0)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    // Lookup before rename on 'to' returns the original rid
    ASSERT_EQ(lookupCatalogId(to, originalUUID, Timestamp(1, 9)).id, originalToRid);
    // Lookup before rename on 'from' returns the rid
    ASSERT_EQ(lookupCatalogId(from, uuid, Timestamp(1, 9)).id, rid);
    // Lookup at rename timestamp on 'to' and 'uuid' returns catalogId
    ASSERT_EQ(lookupCatalogId(to, uuid, Timestamp(1, 10)).id, rid);
    // Lookup at rename timestamp on 'originalUUID' returns not exists as it was dropped during the
    // rename.
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(originalUUID, Timestamp(1, 10)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup after rename on 'to' and 'uuid' returns catalogId
    ASSERT_EQ(lookupCatalogId(to, uuid, Timestamp(1, 20)).id, rid);
    // Lookup after rename timestamp on 'originalUUID' returns not exists as it was dropped during
    // the rename.
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(originalUUID, Timestamp(1, 20)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingDropCreate) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create, drop and recreate collection on the same namespace. We have different catalogId.
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    RecordId rid1 = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 15));
    RecordId rid2 = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();

    // Lookup without timestamp returns latest catalogId
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, boost::none).id, rid2);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(firstUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup before first create returns not exists
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 4)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 4)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup at first create returns first catalogId
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).id, rid1);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup after first create returns first catalogId
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 6)).id, rid1);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(secondUUID, Timestamp(1, 6)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup at drop returns none
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 10)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 10)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup after drop returns none
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 13)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 13)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup at second create returns second catalogId
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).id, rid2);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(firstUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    // Lookup after second create returns second catalogId
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 20)).id, rid2);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(firstUUID, Timestamp(1, 20)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupEqDrop) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create collection and verify we have nothing to cleanup
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 15));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 7)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);

    // Cleanup at drop timestamp
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 10));
    });
    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(firstUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupGtDrop) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create collection and verify we have nothing to cleanup
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 15));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 7)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 12)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);

    // Cleanup after the drop timestamp
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 12));
    });

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 12)));
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(firstUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupGtRecreate) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create collection and verify we have nothing to cleanup
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 15));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 7)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 20)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);

    // Cleanup after the recreate timestamp
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 20));
    });

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 20)));
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByUUID(firstUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupMultiple) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create and drop multiple namespace on the same namespace
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 15));
    dropCollection(opCtx.get(), nss, Timestamp(1, 20));
    UUID thirdUUID = createCollection(opCtx.get(), nss, Timestamp(1, 25));
    dropCollection(opCtx.get(), nss, Timestamp(1, 30));
    UUID fourthUUID = createCollection(opCtx.get(), nss, Timestamp(1, 35));
    dropCollection(opCtx.get(), nss, Timestamp(1, 40));

    // Lookup can find all four collections
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Cleanup oldest
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 10));
    });

    // Lookup can find the three remaining collections
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Cleanup
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 21));
    });

    // Lookup can find the two remaining collections
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Cleanup
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 32));
    });

    // Lookup can find the last remaining collections
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Cleanup
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 50));
    });

    // Lookup now result in unknown as the oldest timestamp has advanced where mapping has been
    // removed
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupMultipleSingleCall) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create and drop multiple namespace on the same namespace
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 5));
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 15));
    dropCollection(opCtx.get(), nss, Timestamp(1, 20));
    UUID thirdUUID = createCollection(opCtx.get(), nss, Timestamp(1, 25));
    dropCollection(opCtx.get(), nss, Timestamp(1, 30));
    UUID fourthUUID = createCollection(opCtx.get(), nss, Timestamp(1, 35));
    dropCollection(opCtx.get(), nss, Timestamp(1, 40));

    // Lookup can find all four collections
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Cleanup all
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 50));
    });

    // Lookup now result in unknown as the oldest timestamp has advanced where mapping has been
    // removed
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 5)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, thirdUUID, Timestamp(1, 25)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, fourthUUID, Timestamp(1, 35)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingRollback) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString a = NamespaceString::createNamespaceString_forTest("b.a");
    NamespaceString b = NamespaceString::createNamespaceString_forTest("b.b");
    NamespaceString c = NamespaceString::createNamespaceString_forTest("b.c");
    NamespaceString d = NamespaceString::createNamespaceString_forTest("b.d");
    NamespaceString e = NamespaceString::createNamespaceString_forTest("b.e");

    // Create and drop multiple namespace on the same namespace
    UUID firstUUID = createCollection(opCtx.get(), a, Timestamp(1, 1));
    dropCollection(opCtx.get(), a, Timestamp(1, 2));
    UUID secondUUID = createCollection(opCtx.get(), a, Timestamp(1, 3));
    UUID thirdUUID = createCollection(opCtx.get(), b, Timestamp(1, 5));
    UUID fourthUUID = createCollection(opCtx.get(), c, Timestamp(1, 7));
    UUID fifthUUID = createCollection(opCtx.get(), d, Timestamp(1, 8));
    UUID sixthUUID = createCollection(opCtx.get(), e, Timestamp(1, 9));
    dropCollection(opCtx.get(), b, Timestamp(1, 10));

    // Rollback to Timestamp(1, 8)
    CollectionCatalog::write(
        opCtx.get(), [&](CollectionCatalog& c) { c.cleanupForCatalogReopen(Timestamp(1, 8)); });

    ASSERT_EQ(lookupCatalogId(e, firstUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(a, secondUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(b, thirdUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(c, fourthUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(d, fifthUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(lookupCatalogId(e, sixthUUID, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingInsert) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create a collection on the namespace
    UUID firstUUID = createCollection(opCtx.get(), nss, Timestamp(1, 10));
    dropCollection(opCtx.get(), nss, Timestamp(1, 20));
    UUID secondUUID = createCollection(opCtx.get(), nss, Timestamp(1, 30));

    auto rid1 = lookupCatalogId(nss, firstUUID, Timestamp(1, 10)).id;
    auto rid2 = lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).id;

    // Simulate startup where we have a range [oldest, stable] by creating and dropping collections
    // and then advancing the oldest timestamp and then reading behind it.
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 40));
    });

    // Confirm that the mappings have been cleaned up
    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);

    {
        OneOffRead oor(opCtx.get(), Timestamp(1, 17));
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, Timestamp(1, 17));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), firstUUID}, Timestamp(1, 17));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), secondUUID}, Timestamp(1, 17));

        // Lookups before the inserted timestamp is still unknown
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 11)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 11)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kUnknown);

        // Lookups at or after the inserted timestamp is found, even if they don't match with WT
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 25)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 25)).id, rid1);
        // The entry at Timestamp(1, 30) is unaffected
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).id, rid2);
    }

    {
        OneOffRead oor(opCtx.get(), Timestamp(1, 12));
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);

        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, Timestamp(1, 12));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), firstUUID}, Timestamp(1, 12));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), secondUUID}, Timestamp(1, 12));

        // We should now have extended the range from Timestamp(1, 17) to Timestamp(1, 12)
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 12)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 12)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 16)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 16)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 25)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 25)).id, rid1);
        // The entry at Timestamp(1, 30) is unaffected
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).id, rid2);
    }

    {
        OneOffRead oor(opCtx.get(), Timestamp(1, 25));
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, Timestamp(1, 25));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), firstUUID}, Timestamp(1, 25));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), secondUUID}, Timestamp(1, 25));

        // Check the entries, most didn't change
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 22)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 22)).id, rid1);
        // At Timestamp(1, 25) we now return kNotExists
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 25)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 25)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
        // But next timestamp returns unknown
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 26)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 26)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
        // The entry at Timestamp(1, 30) is unaffected
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).id, rid2);
    }

    {
        OneOffRead oor(opCtx.get(), Timestamp(1, 25));
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), nss, Timestamp(1, 26));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), firstUUID}, Timestamp(1, 26));
        CollectionCatalog::get(opCtx.get())
            ->establishConsistentCollection(opCtx.get(), {nss.db(), secondUUID}, Timestamp(1, 26));

        // We should not have re-written the existing entry at Timestamp(1, 26)
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 17)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 19)).id, rid1);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 22)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 22)).id, rid1);
        // At Timestamp(1, 25) we now return kNotExists
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 25)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 25)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
        // But next timestamp returns unknown
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 26)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 26)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
        ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 27)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 27)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
        // The entry at Timestamp(1, 30) is unaffected
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).result,
                  CollectionCatalog::CatalogIdLookup::Existence::kExists);
        ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 30)).id, rid2);
    }


    // Clean up, check so we are back to the original state
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 41));
    });

    ASSERT_EQ(lookupCatalogId(nss, firstUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
    ASSERT_EQ(lookupCatalogId(nss, secondUUID, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingInsertUnknown) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Simulate startup where we have a range [oldest, stable] by advancing the oldest timestamp and
    // then reading behind it.
    CollectionCatalog::write(opCtx.get(), [](CollectionCatalog& catalog) {
        catalog.cleanupForOldestTimestampAdvanced(Timestamp(1, 40));
    });

    // Reading before the oldest is unknown
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kUnknown);

    // Try to instantiate a non existing collection at this timestamp.
    CollectionCatalog::get(opCtx.get())
        ->establishConsistentCollection(opCtx.get(), nss, Timestamp(1, 15));

    // Lookup should now be not existing
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogTimestampTest, CollectionLifetimeTiedToStorageTransactionLifetime) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

        auto coll = CollectionCatalog::get(opCtx.get())
                        ->establishConsistentCollection(opCtx.get(), nss, readTimestamp);
        ASSERT(coll);

        std::shared_ptr<const Collection> fetchedColl =
            OpenedCollections::get(opCtx.get()).lookupByNamespace(nss).value();
        ASSERT(fetchedColl);
        ASSERT_EQ(coll, fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        opCtx->recoveryUnit()->abandonSnapshot();
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
        ASSERT_EQ(coll, fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        wuow.commit();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));

        opCtx->recoveryUnit()->abandonSnapshot();
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
        ASSERT_EQ(coll, fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        // The storage snapshot is aborted when the WriteUnitOfWork destructor runs.
        wuow.reset();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));

        opCtx->recoveryUnit()->abandonSnapshot();
        ASSERT(!OpenedCollections::get(opCtx.get()).lookupByNamespace(nss));
    }
}

TEST_F(CollectionCatalogNoTimestampTest, CatalogIdMappingNoTimestamp) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");

    // Create a collection on the namespace and confirm that we can lookup
    UUID uuid = createCollection(opCtx.get(), nss, Timestamp());
    ASSERT_EQ(lookupCatalogId(nss, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Drop the collection and confirm it is also removed from mapping
    dropCollection(opCtx.get(), nss, Timestamp());
    ASSERT_EQ(lookupCatalogId(nss, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogNoTimestampTest, CatalogIdMappingNoTimestampRename) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString a = NamespaceString::createNamespaceString_forTest("a.a");
    NamespaceString b = NamespaceString::createNamespaceString_forTest("a.b");

    // Create a collection on the namespace and confirm that we can lookup
    UUID uuid = createCollection(opCtx.get(), a, Timestamp());
    ASSERT_EQ(lookupCatalogId(a, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(b).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);

    // Rename the collection and check lookup behavior
    renameCollection(opCtx.get(), a, b, Timestamp());
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(a).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(b, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Drop the collection and confirm it is also removed from mapping
    dropCollection(opCtx.get(), b, Timestamp());
    ASSERT_EQ(lookupCatalogId(a, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(b, uuid, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

TEST_F(CollectionCatalogNoTimestampTest, CatalogIdMappingNoTimestampRenameDropTarget) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString a = NamespaceString::createNamespaceString_forTest("a.a");
    NamespaceString b = NamespaceString::createNamespaceString_forTest("a.b");

    // Create collections on the namespaces and confirm that we can lookup
    UUID uuidA = createCollection(opCtx.get(), a, Timestamp());
    UUID uuidB = createCollection(opCtx.get(), b, Timestamp());
    auto [aId, aResult] = lookupCatalogId(a, uuidA, boost::none);
    auto [bId, bResult] = lookupCatalogId(b, uuidB, boost::none);
    ASSERT_EQ(aResult, CollectionCatalog::CatalogIdLookup::Existence::kExists);
    ASSERT_EQ(bResult, CollectionCatalog::CatalogIdLookup::Existence::kExists);

    // Rename the collection and check lookup behavior
    renameCollection(opCtx.get(), a, b, Timestamp());
    auto [aIdAfter, aResultAfter] = catalog()->lookupCatalogIdByNSS(a, boost::none);
    auto [bIdAfter, bResultAfter] = lookupCatalogId(b, uuidA, boost::none);
    ASSERT_EQ(aResultAfter, CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(bResultAfter, CollectionCatalog::CatalogIdLookup::Existence::kExists);
    // Verify that the the recordId on b is now what was on a. We performed a rename with
    // dropTarget=true.
    ASSERT_EQ(aId, bIdAfter);

    // Drop the collection and confirm it is also removed from mapping
    dropCollection(opCtx.get(), b, Timestamp());
    ASSERT_EQ(lookupCatalogId(a, uuidA, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
    ASSERT_EQ(lookupCatalogId(b, uuidB, boost::none).result,
              CollectionCatalog::CatalogIdLookup::Existence::kNotExists);
}

DEATH_TEST_F(CollectionCatalogTimestampTest, OpenCollectionInWriteUnitOfWork, "invariant") {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    // When the snapshot is opened right before the create is committed to the durable catalog, the
    // collection instance should not exist yet.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, boost::none, createCollectionTs, true, false, 0);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateCollectionAndOpenCollectionAfterCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    // When the snapshot is opened right after the create is committed to the durable catalog, the
    // collection instance should exist.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, boost::none, createCollectionTs, false, true, 0);
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentCreateCollectionAndOpenCollectionByUUIDBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    UUID uuid = UUID::gen();

    // When the snapshot is opened right after the create is committed to the durable catalog, the
    // collection instance should exist.
    concurrentCreateCollectionAndEstablishConsistentCollection(
        opCtx.get(), nss, uuid, createCollectionTs, false, true, 0);
}

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropCollectionAndOpenCollectionBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionAndOpenCollectionWithOriginalNameBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString originalNss = NamespaceString::createNamespaceString_forTest("a.b");
    const NamespaceString targetNss = NamespaceString::createNamespaceString_forTest("a.c");
    const Timestamp createOriginalCollectionTs = Timestamp(10, 10);
    const Timestamp createTargetCollectionTs = Timestamp(15, 15);
    const Timestamp renameCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), originalNss, createOriginalCollectionTs);
    createCollection(opCtx.get(), targetNss, createTargetCollectionTs);

    // We expect to find the UUID for the target collection
    UUID uuid = CollectionCatalog::get(opCtx.get())
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

            ASSERT_EQ(CollectionCatalog::get(opCtx.get())->lookupNSSByUUID(opCtx.get(), uuid),
                      targetNss);
        });
}

TEST_F(CollectionCatalogTimestampTest,
       ConcurrentRenameCollectionWithDropTargetAndOpenCollectionWithOriginalUUIDBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionAfterCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionByUUIDBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentCreateIndexAndOpenCollectionByUUIDAfterCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionAfterCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionByUUIDBeforeCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, ConcurrentDropIndexAndOpenCollectionByUUIDAfterCommit) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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

TEST_F(CollectionCatalogTimestampTest, OpenCollectionBetweenIndexBuildInProgressAndReady) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

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
                  coll);
        ASSERT_EQ(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), coll->uuid()),
            coll);
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
                  coll);
        ASSERT_EQ(
            CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), coll->uuid()),
            coll);
    }
}

TEST_F(CollectionCatalogTimestampTest, ResolveNamespaceStringOrUUIDAtLatest) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const UUID uuid = createCollection(opCtx.get(), nss, createCollectionTs);
    const NamespaceStringOrUUID nssOrUUID = NamespaceStringOrUUID(nss.db(), uuid);

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
}  // namespace
}  // namespace mongo
