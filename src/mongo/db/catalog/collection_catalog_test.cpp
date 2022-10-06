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
        col = CollectionPtr(collection.get(), CollectionPtr::NoYieldTag{});
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
            NamespaceString fooNss("foo", "coll" + std::to_string(counter));
            NamespaceString barNss("bar", "coll" + std::to_string(counter));

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
            ASSERT(catalogColl != nullptr);
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
            NamespaceString nss("resourceDb", "coll" + std::to_string(i));
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
        NamespaceString nss("resourceDb", "coll" + std::to_string(i));
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
    const NamespaceString collNs = NamespaceString(boost::none, "resourceDb.coll1");
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    auto ridStr = ResourceCatalog::get(getServiceContext()).name(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(collNs.toStringWithTenantId()) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingCollectionResource) {
    const NamespaceString nss = NamespaceString(boost::none, "resourceDb.coll5");
    auto rid = ResourceId(RESOURCE_COLLECTION, nss);
    ASSERT(!ResourceCatalog::get(getServiceContext()).name(rid));
}

TEST_F(CollectionCatalogResourceTest, RemoveCollection) {
    const NamespaceString collNs = NamespaceString(boost::none, "resourceDb.coll1");
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
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == col);
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
    NamespaceString newNss(nss.db(), "newcol");
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
    auto yieldableColl = catalog.lookupCollectionByUUID(opCtx.get(), colUUID);
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, col);

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
    NamespaceString oldNss(nss.db(), "oldcol");
    std::shared_ptr<Collection> collShared = std::make_shared<CollectionMock>(uuid, oldNss);
    auto collection = collShared.get();
    catalog.registerCollection(opCtx.get(), uuid, std::move(collShared), boost::none);
    auto yieldableColl = catalog.lookupCollectionByUUID(opCtx.get(), uuid);
    ASSERT(yieldableColl);
    ASSERT_EQUALS(yieldableColl, collection);

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
    ASSERT_EQUALS(yieldableColl, collection);

    // Reset CollectionPtr for post-rename restore test.
    yieldableColl.yield();
    ASSERT_FALSE(yieldableColl);

    NamespaceString newNss(NamespaceString(nss.db(), "newcol"));
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
    NamespaceString newNss(nss.db(), "newcol");
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
    NamespaceString newNss(nss.db(), "newcol");
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
    NamespaceString aColl("dbA", "collA");
    NamespaceString b1Coll("dbB", "collB1");
    NamespaceString b2Coll("dbB", "collB2");
    NamespaceString cColl("dbC", "collC");
    NamespaceString d1Coll("dbD", "collD1");
    NamespaceString d2Coll("dbD", "collD2");
    NamespaceString d3Coll("dbD", "collD3");

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
    NamespaceString dbA(tid1, "dbA.collA");
    NamespaceString dbB(tid1, "dbB.collA");
    NamespaceString dbC(tid1, "dbC.collA");
    NamespaceString dbD(tid2, "dbB.collA");

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
    NamespaceString aColl("dbA", "collA");
    NamespaceString b1Coll("dbB", "collB1");
    NamespaceString b2Coll("dbB", "collB2");
    NamespaceString cColl("dbC", "collC");
    NamespaceString d1Coll("dbD", "collD1");
    NamespaceString d2Coll("dbD", "collD2");
    NamespaceString d3Coll("dbD", "collD3");

    std::vector<NamespaceString> nsss = {aColl, b1Coll, b2Coll, cColl, d1Coll, d2Coll, d3Coll};
    for (auto& nss : nsss) {
        std::shared_ptr<Collection> newColl = std::make_shared<CollectionMock>(nss);
        auto uuid = UUID::gen();
        catalog.registerCollection(opCtx.get(), uuid, std::move(newColl), boost::none);
    }

    // One dbName with only an invisible collection does not appear in dbNames. Use const_cast to
    // modify the collection in the catalog inplace, this bypasses copy-on-write behavior.
    auto invisibleCollA =
        const_cast<Collection*>(catalog.lookupCollectionByNamespace(opCtx.get(), aColl).get());
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
            const_cast<Collection*>(catalog.lookupCollectionByNamespace(opCtx.get(), nss).get());
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
            const_cast<Collection*>(catalog.lookupCollectionByNamespace(opCtx.get(), nss).get());
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
            operationContext(), NamespaceString("db", "coll1"), emptyCollOptions));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), NamespaceString("db", "coll2"), tempCollOptions));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), NamespaceString("db", "coll3"), tempCollOptions));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), NamespaceString("db2", "coll4"), emptyCollOptions));
    }
};

TEST_F(ForEachCollectionFromDbTest, ForEachCollectionFromDb) {
    createTestData();
    auto opCtx = operationContext();

    {
        const DatabaseName dbName(boost::none, "db");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx, dbName, MODE_X, [&](const CollectionPtr& collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
                numCollectionsTraversed++;
                return true;
            });

        ASSERT_EQUALS(numCollectionsTraversed, 3);
    }

    {
        const DatabaseName dbName(boost::none, "db2");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx, dbName, MODE_IS, [&](const CollectionPtr& collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IS));
                numCollectionsTraversed++;
                return true;
            });

        ASSERT_EQUALS(numCollectionsTraversed, 1);
    }

    {
        const DatabaseName dbName(boost::none, "db3");
        auto dbLock = std::make_unique<Lock::DBLock>(opCtx, dbName, MODE_IX);
        int numCollectionsTraversed = 0;
        catalog::forEachCollectionFromDb(
            opCtx, dbName, MODE_S, [&](const CollectionPtr& collection) {
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
            [&](const CollectionPtr& collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
                numCollectionsTraversed++;
                return true;
            },
            [&](const CollectionPtr& collection) {
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
            [&](const CollectionPtr& collection) {
                ASSERT_TRUE(
                    opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IX));
                numCollectionsTraversed++;
                return true;
            },
            [&](const CollectionPtr& collection) {
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

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        opCtx = makeOperationContext();
    }

    std::shared_ptr<const CollectionCatalog> catalog() {
        return CollectionCatalog::get(opCtx.get());
    }

    void createCollection(OperationContext* opCtx,
                          const NamespaceString& nss,
                          Timestamp timestamp) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        opCtx->recoveryUnit()->abandonSnapshot();

        if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
        opCtx->recoveryUnit()->setCommitTimestamp(timestamp);

        AutoGetDb databaseWriteGuard(opCtx, nss.dbName(), MODE_IX);
        auto db = databaseWriteGuard.ensureDbExists(opCtx);
        ASSERT(db);

        Lock::CollectionLock lk(opCtx, nss, MODE_IX);

        WriteUnitOfWork wuow(opCtx);
        CollectionOptions options;
        options.uuid.emplace(UUID::gen());

        auto storageEngine = getServiceContext()->getStorageEngine();
        std::pair<RecordId, std::unique_ptr<RecordStore>> catalogIdRecordStorePair =
            uassertStatusOK(storageEngine->getCatalog()->createCollection(
                opCtx, nss, options, /*allocateDefaultSpace=*/true));
        auto& catalogId = catalogIdRecordStorePair.first;
        std::shared_ptr<Collection> ownedCollection = Collection::Factory::get(opCtx)->make(
            opCtx, nss, catalogId, options, std::move(catalogIdRecordStorePair.second));
        ownedCollection->init(opCtx);
        ownedCollection->setCommitted(false);

        CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, std::move(ownedCollection));

        wuow.commit();
    }

    void dropCollection(OperationContext* opCtx, const NamespaceString& nss, Timestamp timestamp) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        opCtx->recoveryUnit()->abandonSnapshot();

        if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
        opCtx->recoveryUnit()->setCommitTimestamp(timestamp);

        Lock::DBLock dbLk(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLk(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);

        WriteUnitOfWork wuow(opCtx);

        // Add the collection ident to the drop-pending reaper.
        opCtx->getServiceContext()->getStorageEngine()->addDropPendingIdent(
            timestamp, collection->getRecordStore()->getSharedIdent());

        CollectionCatalog::get(opCtx)->dropCollection(
            opCtx, collection.getWritableCollection(opCtx), /*isDropPending=*/true);
        wuow.commit();
    }

    void renameCollection(OperationContext* opCtx,
                          const NamespaceString& from,
                          const NamespaceString& to,
                          Timestamp timestamp) {
        invariant(from.db() == to.db());

        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        opCtx->recoveryUnit()->abandonSnapshot();

        if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
        opCtx->recoveryUnit()->setCommitTimestamp(timestamp);

        Lock::DBLock dbLk(opCtx, from.db(), MODE_IX);
        Lock::CollectionLock fromLk(opCtx, from, MODE_X);
        Lock::CollectionLock toLk(opCtx, to, MODE_X);

        CollectionWriter collection(opCtx, from);

        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection.getWritableCollection(opCtx)->rename(opCtx, to, false));
        CollectionCatalog::get(opCtx)->onCollectionRename(
            opCtx, collection.getWritableCollection(opCtx), from);
        wuow.commit();
    }

    void createIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     BSONObj indexSpec,
                     Timestamp timestamp) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        opCtx->recoveryUnit()->abandonSnapshot();

        if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
        opCtx->recoveryUnit()->setCommitTimestamp(timestamp);

        AutoGetCollection autoColl(opCtx, nss, MODE_X);

        WriteUnitOfWork wuow(opCtx);
        CollectionWriter collection(opCtx, nss);

        IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
            opCtx, collection, {indexSpec}, /*fromMigrate=*/false);

        wuow.commit();
    }

    void dropIndex(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const std::string& indexName,
                   Timestamp timestamp) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        opCtx->recoveryUnit()->abandonSnapshot();

        if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
        opCtx->recoveryUnit()->setCommitTimestamp(timestamp);

        AutoGetCollection autoColl(opCtx, nss, MODE_X);

        WriteUnitOfWork wuow(opCtx);
        CollectionWriter collection(opCtx, nss);

        Collection* writableCollection = collection.getWritableCollection(opCtx);

        IndexCatalog* indexCatalog = writableCollection->getIndexCatalog();
        auto indexDescriptor =
            indexCatalog->findIndexByName(opCtx, indexName, IndexCatalog::InclusionPolicy::kReady);

        // This also adds the index ident to the drop-pending reaper.
        ASSERT_OK(indexCatalog->dropIndex(opCtx, writableCollection, indexDescriptor));

        wuow.commit();
    }

protected:
    ServiceContext::UniqueOperationContext opCtx;
};

TEST_F(CollectionCatalogTimestampTest, MinimumValidSnapshot) {
    const NamespaceString nss("a.b");
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

    const NamespaceString nss("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);

    createCollection(opCtx.get(), nss, createCollectionTs);

    // Try to open the collection before it was created.
    const Timestamp readTimestamp(5, 5);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    OneOffRead oor(opCtx.get(), readTimestamp);
    auto coll =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(!coll);
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
    std::shared_ptr<Collection> coll =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);
    ASSERT_EQ(0, coll->getIndexCatalog()->numIndexesTotal(opCtx.get()));

    // Verify that the CollectionCatalog returns the latest collection with the index present. This
    // has to be done in an alternative client as we already have an open snapshot from an earlier
    // point-in-time above.
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl = CollectionCatalog::get(newOpCtx.get())
                          ->lookupCollectionByNamespaceForRead(newOpCtx.get(), nss);
    ASSERT(latestColl);
    ASSERT_EQ(1, latestColl->getIndexCatalog()->numIndexesTotal(newOpCtx.get()));

    // Ensure the idents are shared between the collection instances.
    ASSERT_NE(coll, latestColl);
    ASSERT_EQ(coll->getSharedIdent(), latestColl->getSharedIdent());
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollectionWithIndex) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
    std::shared_ptr<Collection> coll =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);
    ASSERT_EQ(1, coll->getIndexCatalog()->numIndexesTotal(opCtx.get()));

    // Verify that the CollectionCatalog returns the latest collection. This has to be done in an
    // alternative client as we already have an open snapshot from an earlier point-in-time above.
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl = CollectionCatalog::get(newOpCtx.get())
                          ->lookupCollectionByNamespaceForRead(newOpCtx.get(), nss);
    ASSERT(latestColl);
    ASSERT_EQ(2, latestColl->getIndexCatalog()->numIndexesTotal(newOpCtx.get()));

    // Ensure the idents are shared between the collection and index instances.
    ASSERT_NE(coll, latestColl);
    ASSERT_EQ(coll->getSharedIdent(), latestColl->getSharedIdent());

    auto indexDescPast = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescLatest = latestColl->getIndexCatalog()->findIndexByName(newOpCtx.get(), "x_1");
    ASSERT_BSONOBJ_EQ(indexDescPast->infoObj(), indexDescLatest->infoObj());
    ASSERT_EQ(coll->getIndexCatalog()->getEntryShared(indexDescPast),
              latestColl->getIndexCatalog()->getEntryShared(indexDescLatest));
}

TEST_F(CollectionCatalogTimestampTest, OpenLatestCollectionWithIndex) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
    std::shared_ptr<Collection> coll =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);

    // Verify that the CollectionCatalog returns the latest collection.
    auto currentColl =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespaceForRead(opCtx.get(), nss);
    ASSERT_EQ(coll, currentColl);

    // Ensure the idents are shared between the collection and index instances.
    ASSERT_EQ(coll->getSharedIdent(), currentColl->getSharedIdent());

    auto indexDesc = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescCurrent = currentColl->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    ASSERT_BSONOBJ_EQ(indexDesc->infoObj(), indexDescCurrent->infoObj());
    ASSERT_EQ(coll->getIndexCatalog()->getEntryShared(indexDesc),
              currentColl->getIndexCatalog()->getEntryShared(indexDescCurrent));
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierCollectionWithDropPendingIndex) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
        auto latestColl = CollectionCatalog::get(opCtx.get())
                              ->lookupCollectionByNamespaceForRead(opCtx.get(), nss);
        auto desc = latestColl->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
        index = latestColl->getIndexCatalog()->getEntryShared(desc);
    }

    dropIndex(opCtx.get(), nss, "x_1", dropIndexTs);
    dropIndex(opCtx.get(), nss, "y_1", dropIndexTs);

    // Open the collection while both indexes were present.
    const Timestamp readTimestamp(20, 20);
    OneOffRead oor(opCtx.get(), readTimestamp);
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    std::shared_ptr<Collection> coll =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(coll);

    // Collection is not shared from the latest instance. This has to be done in an  alternative
    // client as we already have an open snapshot from an earlier point-in-time above.
    auto newClient = opCtx->getServiceContext()->makeClient("AlternativeClient");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    auto latestColl = CollectionCatalog::get(newOpCtx.get())
                          ->lookupCollectionByNamespaceForRead(newOpCtx.get(), nss);
    ASSERT_NE(coll, latestColl);

    auto indexDescX = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "x_1");
    auto indexDescY = coll->getIndexCatalog()->findIndexByName(opCtx.get(), "y_1");

    auto indexEntryX = coll->getIndexCatalog()->getEntryShared(indexDescX);
    auto indexEntryY = coll->getIndexCatalog()->getEntryShared(indexDescY);

    // Check use_count(). 2 in the unit test, 1 in the opened collection.
    ASSERT_EQ(3, indexEntryX.use_count());

    // Check use_count(). 1 in the unit test, 1 in the opened collection.
    ASSERT_EQ(2, indexEntryY.use_count());

    // Verify that "x_1" was retrieved from the drop pending map for the opened collection.
    ASSERT_EQ(index, indexEntryX);
}

TEST_F(CollectionCatalogTimestampTest, OpenEarlierAlreadyDropPendingCollection) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString firstNss("a.b");
    const NamespaceString secondNss("c.d");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(30, 30);

    createCollection(opCtx.get(), firstNss, createCollectionTs);
    createCollection(opCtx.get(), secondNss, createCollectionTs);

    // Maintain a shared_ptr to "a.b" so that it isn't expired in the drop pending map after we drop
    // the collections.
    std::shared_ptr<const Collection> coll =
        CollectionCatalog::get(opCtx.get())
            ->lookupCollectionByNamespaceForRead(opCtx.get(), firstNss);
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
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())
                ->openCollection(opCtx.get(), firstNss, readTimestamp);
        ASSERT(openedColl);
        ASSERT_EQ(coll, openedColl);

        // Check use_count(). 2 in the unit test, 1 in UncommittedCatalogUpdates.
        ASSERT_EQ(3, openedColl.use_count());

        // Check use_count(). 2 in the unit test.
        opCtx->recoveryUnit()->abandonSnapshot();
        ASSERT_EQ(2, openedColl.use_count());
    }

    {
        OneOffRead oor(opCtx.get(), readTimestamp);

        // Open "c.d" which is expired in the drop pending map.
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())
                ->openCollection(opCtx.get(), secondNss, readTimestamp);
        ASSERT(openedColl);
        ASSERT_NE(coll, openedColl);

        // Check use_count(). 1 in the unit test, 1 in UncommittedCatalogUpdates.
        ASSERT_EQ(2, openedColl.use_count());

        // Check use_count(). 1 in the unit test.
        opCtx->recoveryUnit()->abandonSnapshot();
        ASSERT_EQ(1, openedColl.use_count());
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionUsingDropPendingCollectionSharedState) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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

    // Maintain a shared_ptr to "a.b" so that it isn't expired in the drop pending map after we drop
    // it.
    std::shared_ptr<const Collection> coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespaceForRead(opCtx.get(), nss);
    ASSERT(coll);
    ASSERT_EQ(*coll->getMinimumValidSnapshot(), createIndexTs);

    // Make the collection drop pending.
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    // Open the collection before the index was created. The drop pending collection is incompatible
    // as it has an index entry. But we can still use the drop pending collections shared state to
    // instantiate a new collection.
    const Timestamp readTimestamp(10, 10);
    OneOffRead oor(opCtx.get(), readTimestamp);

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    std::shared_ptr<Collection> openedColl =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
    ASSERT(openedColl);
    ASSERT_NE(coll, openedColl);

    // Check use_count(). 1 in the unit test, 1 in UncommittedCatalogUpdates.
    ASSERT_EQ(2, openedColl.use_count());

    // Ensure the idents are shared between the opened collection and the drop pending collection.
    ASSERT_EQ(coll->getSharedIdent(), openedColl->getSharedIdent());

    // Check use_count(). 1 in the unit test.
    opCtx->recoveryUnit()->abandonSnapshot();
    ASSERT_EQ(1, openedColl.use_count());
}

TEST_F(CollectionCatalogTimestampTest, OpenExistingCollectionWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
    const Timestamp createCollectionTs = Timestamp(10, 10);
    const Timestamp dropCollectionTs = Timestamp(20, 20);

    createCollection(opCtx.get(), nss, createCollectionTs);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Maintain a shared_ptr so that the reaper cannot drop the collection ident.
    std::shared_ptr<const Collection> coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespaceForRead(opCtx.get(), nss);
    ASSERT(coll);

    // Mark the collection as drop pending. The dropToken in the ident reaper is not expired as we
    // still have a reference.
    dropCollection(opCtx.get(), nss, dropCollectionTs);

    {
        ASSERT_EQ(1, storageEngine->getDropPendingIdents().size());
        ASSERT_EQ(coll->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());

        // Ident is not expired and should not be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());

        ASSERT_EQ(1, storageEngine->getDropPendingIdents().size());
        ASSERT_EQ(coll->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());
    }

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
    std::shared_ptr<Collection> openedColl =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, createCollectionTs);
    ASSERT(openedColl);
    ASSERT_EQ(coll->getSharedIdent(), openedColl->getSharedIdent());

    // The ident is now expired and should be removed the next time the ident reaper runs.
    coll.reset();
    openedColl.reset();

    // Remove the collection reference in UncommittedCatalogUpdates.
    opCtx->recoveryUnit()->abandonSnapshot();

    storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
    ASSERT_EQ(0, storageEngine->getDropPendingIdents().size());

    // Now we fail to open the collection as the ident has been removed.
    OneOffRead oor(opCtx.get(), createCollectionTs);
    ASSERT(
        !CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, createCollectionTs));
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())
                ->openCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);

        ASSERT_EQ(1, storageEngine->getDropPendingIdents().size());
        ASSERT_EQ(openedColl->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());

        // Ident is marked as in use and it should not be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());

        ASSERT_EQ(1, storageEngine->getDropPendingIdents().size());
        ASSERT_EQ(openedColl->getRecordStore()->getSharedIdent()->getIdent(),
                  *storageEngine->getDropPendingIdents().begin());
    }

    {
        // Run the ident reaper before opening the collection.
        ASSERT_EQ(1, storageEngine->getDropPendingIdents().size());

        // The dropToken is expired as the ident is no longer in use.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());

        ASSERT_EQ(0, storageEngine->getDropPendingIdents().size());

        // Now we fail to open the collection as the ident has been removed.
        OneOffRead oor(opCtx.get(), createCollectionTs);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->openCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenExistingCollectionAndIndexesWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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

    // Maintain a shared_ptr to the collection so that the reaper cannot drop the collection ident.
    std::shared_ptr<const Collection> coll =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByNamespaceForRead(opCtx.get(), nss);
    ASSERT(coll);

    dropCollection(opCtx.get(), nss, dropCollectionTs);

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    ASSERT_EQ(3, storageEngine->getDropPendingIdents().size());

    {
        // Open the collection using shared state before any index drops.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, createIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(openedColl->getSharedIdent(), coll->getSharedIdent());
        ASSERT_EQ(2, openedColl->getIndexCatalog()->numIndexesTotal(opCtx.get()));

        // All idents are marked as in use and none should be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(3, storageEngine->getDropPendingIdents().size());
    }

    {
        // Open the collection using shared state after a single index was dropped.
        OneOffRead oor(opCtx.get(), dropXIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, dropXIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(openedColl->getSharedIdent(), coll->getSharedIdent());
        ASSERT_EQ(1, openedColl->getIndexCatalog()->numIndexesTotal(opCtx.get()));

        std::vector<std::string> indexNames;
        openedColl->getAllIndexes(&indexNames);
        ASSERT_EQ(1, indexNames.size());
        ASSERT_EQ("y_1", indexNames.front());

        // Only the collection and 'y' index idents are marked as in use. The 'x' index ident will
        // be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(2, storageEngine->getDropPendingIdents().size());
    }

    {
        // Open the collection using shared state before any indexes were created.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())
                ->openCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);
        ASSERT_EQ(openedColl->getSharedIdent(), coll->getSharedIdent());
        ASSERT_EQ(0, openedColl->getIndexCatalog()->numIndexesTotal(opCtx.get()));
    }

    {
        // Try to open the collection using shared state when both indexes were present. This should
        // fail as the ident for index 'x' was already removed.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(
            !CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, createIndexTs));

        ASSERT_EQ(2, storageEngine->getDropPendingIdents().size());
    }

    {
        // Drop all remaining idents.
        coll.reset();

        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(0, storageEngine->getDropPendingIdents().size());

        // All idents are removed so opening the collection before any indexes were created should
        // fail.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->openCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, OpenNewCollectionAndIndexesWithReaper) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
    ASSERT_EQ(3, storageEngine->getDropPendingIdents().size());

    {
        // Open the collection before any index drops.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, createIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(2, openedColl->getIndexCatalog()->numIndexesTotal(opCtx.get()));

        // All idents are marked as in use and none should be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(3, storageEngine->getDropPendingIdents().size());
    }

    {
        // Open the collection after the 'x' index was dropped.
        OneOffRead oor(opCtx.get(), dropXIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, dropXIndexTs);
        ASSERT(openedColl);
        ASSERT_EQ(1, openedColl->getIndexCatalog()->numIndexesTotal(opCtx.get()));

        std::vector<std::string> indexNames;
        openedColl->getAllIndexes(&indexNames);
        ASSERT_EQ(1, indexNames.size());
        ASSERT_EQ("y_1", indexNames.front());

        // The 'x' index ident will be removed.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(2, storageEngine->getDropPendingIdents().size());
    }

    {
        // Open the collection before any indexes were created.
        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> openedColl =
            CollectionCatalog::get(opCtx.get())
                ->openCollection(opCtx.get(), nss, createCollectionTs);
        ASSERT(openedColl);
        ASSERT_EQ(0, openedColl->getIndexCatalog()->numIndexesTotal(opCtx.get()));
    }

    {
        // Try to open the collection before any index drops. Because the 'x' index ident is already
        // dropped, this should fail.
        OneOffRead oor(opCtx.get(), createIndexTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(
            !CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, createIndexTs));

        ASSERT_EQ(2, storageEngine->getDropPendingIdents().size());
    }

    {
        // Drop all remaining idents and try to open the collection. This should fail.
        storageEngine->dropIdentsOlderThan(opCtx.get(), Timestamp::max());
        ASSERT_EQ(0, storageEngine->getDropPendingIdents().size());

        OneOffRead oor(opCtx.get(), createCollectionTs);

        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT(!CollectionCatalog::get(opCtx.get())
                    ->openCollection(opCtx.get(), nss, createCollectionTs));
    }
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCreate) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create collection and extract the catalogId
    createCollection(opCtx.get(), nss, Timestamp(1, 2));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();

    // Lookup without timestamp returns latest catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, boost::none), rid);
    // Lookup before create returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 1)), boost::none);
    // Lookup at create returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 2)), rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 3)), rid);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingDrop) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create and drop collection. We have a time window where the namespace exists
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));

    // Lookup without timestamp returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, boost::none), boost::none);
    // Lookup before create returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 4)), boost::none);
    // Lookup at create returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 6)), rid);
    // Lookup at drop returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 10)), boost::none);
    // Lookup after drop returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 20)), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingRename) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString from("a.b");
    NamespaceString to("a.c");

    // Create and rename collection. We have two windows where the namespace exists but for
    // different namespaces
    createCollection(opCtx.get(), from, Timestamp(1, 5));
    RecordId rid = catalog()->lookupCollectionByNamespace(opCtx.get(), from)->getCatalogId();
    renameCollection(opCtx.get(), from, to, Timestamp(1, 10));

    // Lookup without timestamp on 'from' returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, boost::none), boost::none);
    // Lookup before create returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 4)), boost::none);
    // Lookup at create returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 5)), rid);
    // Lookup after create returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 6)), rid);
    // Lookup at rename on 'from' returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 10)), boost::none);
    // Lookup after rename on 'from' returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(from, Timestamp(1, 20)), boost::none);

    // Lookup without timestamp on 'to' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(to, boost::none), rid);
    // Lookup before rename on 'to' returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(to, Timestamp(1, 9)), boost::none);
    // Lookup at rename on 'to' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(to, Timestamp(1, 10)), rid);
    // Lookup after rename on 'to' returns catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(to, Timestamp(1, 20)), rid);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingDropCreate) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create, drop and recreate collection on the same namespace. We have different catalogId.
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    RecordId rid1 = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    createCollection(opCtx.get(), nss, Timestamp(1, 15));
    RecordId rid2 = catalog()->lookupCollectionByNamespace(opCtx.get(), nss)->getCatalogId();

    // Lookup without timestamp returns latest catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, boost::none), rid2);
    // Lookup before first create returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 4)), boost::none);
    // Lookup at first create returns first catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), rid1);
    // Lookup after first create returns first catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 6)), rid1);
    // Lookup at drop returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 10)), boost::none);
    // Lookup after drop returns none
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 13)), boost::none);
    // Lookup at second create returns second catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), rid2);
    // Lookup after second create returns second catalogId
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 20)), rid2);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupEqDrop) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create collection and verify we have nothing to cleanup
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    createCollection(opCtx.get(), nss, Timestamp(1, 15));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 7)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);

    // Cleanup at drop timestamp
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 10));
    });
    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupGtDrop) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create collection and verify we have nothing to cleanup
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    createCollection(opCtx.get(), nss, Timestamp(1, 15));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 7)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 12)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);

    // Cleanup after the drop timestamp
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 12));
    });

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 12)));
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupGtRecreate) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create collection and verify we have nothing to cleanup
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));

    // Drop collection and verify we have nothing to cleanup as long as the oldest timestamp is
    // before the drop
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 10)));

    // Create new collection and nothing changed with answers to needsCleanupForOldestTimestamp.
    createCollection(opCtx.get(), nss, Timestamp(1, 15));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 1)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 5)));
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 7)));
    ASSERT_TRUE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 20)));

    // We can lookup the old catalogId before we advance the oldest timestamp and cleanup
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);

    // Cleanup after the recreate timestamp
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 20));
    });

    // After cleanup, we cannot find the old catalogId anymore. Also verify that we don't need
    // anymore cleanup
    ASSERT_FALSE(catalog()->needsCleanupForOldestTimestamp(Timestamp(1, 20)));
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupMultiple) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create and drop multiple namespace on the same namespace
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    createCollection(opCtx.get(), nss, Timestamp(1, 15));
    dropCollection(opCtx.get(), nss, Timestamp(1, 20));
    createCollection(opCtx.get(), nss, Timestamp(1, 25));
    dropCollection(opCtx.get(), nss, Timestamp(1, 30));
    createCollection(opCtx.get(), nss, Timestamp(1, 35));
    dropCollection(opCtx.get(), nss, Timestamp(1, 40));

    // Lookup can find all four collections
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);

    // Cleanup oldest
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 10));
    });

    // Lookup can find the three remaining collections
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);

    // Cleanup
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 21));
    });

    // Lookup can find the two remaining collections
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);

    // Cleanup
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 32));
    });

    // Lookup can find the last remaining collections
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);

    // Cleanup
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 50));
    });

    // Lookup can find no collections
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingCleanupMultipleSingleCall) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString nss("a.b");

    // Create and drop multiple namespace on the same namespace
    createCollection(opCtx.get(), nss, Timestamp(1, 5));
    dropCollection(opCtx.get(), nss, Timestamp(1, 10));
    createCollection(opCtx.get(), nss, Timestamp(1, 15));
    dropCollection(opCtx.get(), nss, Timestamp(1, 20));
    createCollection(opCtx.get(), nss, Timestamp(1, 25));
    dropCollection(opCtx.get(), nss, Timestamp(1, 30));
    createCollection(opCtx.get(), nss, Timestamp(1, 35));
    dropCollection(opCtx.get(), nss, Timestamp(1, 40));

    // Lookup can find all four collections
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);

    // Cleanup all
    CollectionCatalog::write(opCtx.get(), [&](CollectionCatalog& c) {
        c.cleanupForOldestTimestampAdvanced(Timestamp(1, 50));
    });

    // Lookup can find no collections
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 5)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 15)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 25)), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(nss, Timestamp(1, 35)), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CatalogIdMappingRollback) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    NamespaceString a("b.a");
    NamespaceString b("b.b");
    NamespaceString c("b.c");
    NamespaceString d("b.d");
    NamespaceString e("b.e");

    // Create and drop multiple namespace on the same namespace
    createCollection(opCtx.get(), a, Timestamp(1, 1));
    dropCollection(opCtx.get(), a, Timestamp(1, 2));
    createCollection(opCtx.get(), a, Timestamp(1, 3));
    createCollection(opCtx.get(), b, Timestamp(1, 5));
    createCollection(opCtx.get(), c, Timestamp(1, 7));
    createCollection(opCtx.get(), d, Timestamp(1, 8));
    createCollection(opCtx.get(), e, Timestamp(1, 9));
    dropCollection(opCtx.get(), b, Timestamp(1, 10));

    // Rollback to Timestamp(1, 8)
    CollectionCatalog::write(
        opCtx.get(), [&](CollectionCatalog& c) { c.cleanupForCatalogReopen(Timestamp(1, 8)); });

    ASSERT_NE(catalog()->lookupCatalogIdByNSS(a, boost::none), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(b, boost::none), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(c, boost::none), boost::none);
    ASSERT_NE(catalog()->lookupCatalogIdByNSS(d, boost::none), boost::none);
    ASSERT_EQ(catalog()->lookupCatalogIdByNSS(e, boost::none), boost::none);
}

TEST_F(CollectionCatalogTimestampTest, CollectionLifetimeTiedToStorageTransactionLifetime) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx.get());

    {
        // Test that the collection is released when the storage snapshot is abandoned.
        OneOffRead oor(opCtx.get(), readTimestamp);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> coll =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
        ASSERT(coll);

        std::shared_ptr<Collection> fetchedColl =
            uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection;
        ASSERT(fetchedColl);
        ASSERT_EQ(coll.get(), fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        opCtx->recoveryUnit()->abandonSnapshot();
        ASSERT(!uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection);
    }

    {
        // Test that the collection is released when the storage snapshot is committed.
        OneOffRead oor(opCtx.get(), readTimestamp);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> coll =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
        ASSERT(coll);

        WriteUnitOfWork wuow(opCtx.get());

        std::shared_ptr<Collection> fetchedColl =
            uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection;
        ASSERT(fetchedColl);
        ASSERT_EQ(coll.get(), fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        wuow.commit();
        ASSERT(!uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection);

        opCtx->recoveryUnit()->abandonSnapshot();
        ASSERT(!uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection);
    }

    {
        // Test that the collection is released when the storage snapshot is aborted.
        OneOffRead oor(opCtx.get(), readTimestamp);
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        std::shared_ptr<Collection> coll =
            CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
        ASSERT(coll);

        boost::optional<WriteUnitOfWork> wuow(opCtx.get());

        std::shared_ptr<Collection> fetchedColl =
            uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection;
        ASSERT(fetchedColl);
        ASSERT_EQ(coll.get(), fetchedColl.get());
        ASSERT_EQ(coll->getSharedIdent(), fetchedColl->getSharedIdent());

        // The storage snapshot is aborted when the WriteUnitOfWork destructor runs.
        wuow.reset();
        ASSERT(!uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection);

        opCtx->recoveryUnit()->abandonSnapshot();
        ASSERT(!uncommittedCatalogUpdates.lookupCollection(opCtx.get(), nss).collection);
    }
}

DEATH_TEST_F(CollectionCatalogTimestampTest, OpenCollectionInWriteUnitOfWork, "invariant") {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagPointInTimeCatalogLookups", true);

    const NamespaceString nss("a.b");
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
    std::shared_ptr<Collection> coll =
        CollectionCatalog::get(opCtx.get())->openCollection(opCtx.get(), nss, readTimestamp);
}

}  // namespace
}  // namespace mongo
