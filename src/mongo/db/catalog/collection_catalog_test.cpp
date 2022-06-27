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
#include <boost/optional/optional_io.hpp>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context_d_test_fixture.h"
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

        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(colUUID, nss);
        col = CollectionPtr(collection.get(), CollectionPtr::NoYieldTag{});
        // Register dummy collection in catalog.
        catalog.registerCollection(opCtx.get(), colUUID, collection);

        // Validate that kNumCollectionReferencesStored is correct, add one reference for the one we
        // hold in this function.
        ASSERT_EQUALS(collection.use_count(),
                      CollectionCatalog::kNumCollectionReferencesStored + 1);
    }

protected:
    std::shared_ptr<CollectionCatalog> sharedCatalog = std::make_shared<CollectionCatalog>();
    CollectionCatalog& catalog = *sharedCatalog;
    ServiceContext::UniqueOperationContext opCtx;
    NamespaceString nss;
    CollectionPtr col;
    UUID colUUID;
    UUID nextUUID;
    UUID prevUUID;
};

class CollectionCatalogIterationTest : public unittest::Test {
public:
    void setUp() {
        for (int counter = 0; counter < 5; ++counter) {
            NamespaceString fooNss("foo", "coll" + std::to_string(counter));
            NamespaceString barNss("bar", "coll" + std::to_string(counter));

            auto fooUuid = UUID::gen();
            std::shared_ptr<Collection> fooColl = std::make_shared<CollectionMock>(fooNss);

            auto barUuid = UUID::gen();
            std::shared_ptr<Collection> barColl = std::make_shared<CollectionMock>(barNss);

            dbMap["foo"].insert(std::make_pair(fooUuid, fooColl.get()));
            dbMap["bar"].insert(std::make_pair(barUuid, barColl.get()));

            catalog.registerCollection(&opCtx, fooUuid, fooColl);
            catalog.registerCollection(&opCtx, barUuid, barColl);
        }
    }

    void tearDown() {
        for (auto& it : dbMap) {
            for (auto& kv : it.second) {
                catalog.deregisterCollection(&opCtx, kv.first);
            }
        }
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
                 std::tuple{collsIterator(dbName.toString()), catalog.begin(&opCtx, dbName)};
             catalogIt != catalog.end(&opCtx) && orderedIt != collsIteratorEnd(dbName.toString());
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
    OperationContextNoop opCtx;
    std::map<std::string, std::map<UUID, CollectionPtr>> dbMap;
};

class CollectionCatalogResourceMapTest : public unittest::Test {
public:
    void setUp() {
        // The first and second collection namespaces map to the same ResourceId.
        firstCollection = "1661880728";
        secondCollection = "1626936312";

        firstResourceId = ResourceId(RESOURCE_COLLECTION, firstCollection);
        secondResourceId = ResourceId(RESOURCE_COLLECTION, secondCollection);
        ASSERT_EQ(firstResourceId, secondResourceId);

        thirdCollection = "2930102946";
        thirdResourceId = ResourceId(RESOURCE_COLLECTION, thirdCollection);
        ASSERT_NE(firstResourceId, thirdResourceId);
    }

protected:
    std::string firstCollection;
    ResourceId firstResourceId;

    std::string secondCollection;
    ResourceId secondResourceId;

    std::string thirdCollection;
    ResourceId thirdResourceId;

    CollectionCatalog catalog;
};

TEST_F(CollectionCatalogResourceMapTest, EmptyTest) {
    boost::optional<std::string> resource = catalog.lookupResourceName(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.removeResource(secondResourceId, secondCollection);
    resource = catalog.lookupResourceName(secondResourceId);
    ASSERT_EQ(boost::none, resource);
}

TEST_F(CollectionCatalogResourceMapTest, InsertTest) {
    catalog.addResource(firstResourceId, firstCollection);
    boost::optional<std::string> resource = catalog.lookupResourceName(thirdResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.addResource(thirdResourceId, thirdCollection);

    resource = catalog.lookupResourceName(firstResourceId);
    ASSERT_EQ(firstCollection, *resource);

    resource = catalog.lookupResourceName(thirdResourceId);
    ASSERT_EQ(thirdCollection, resource);
}

TEST_F(CollectionCatalogResourceMapTest, RemoveTest) {
    catalog.addResource(firstResourceId, firstCollection);
    catalog.addResource(thirdResourceId, thirdCollection);

    // This fails to remove the resource because of an invalid namespace.
    catalog.removeResource(firstResourceId, "BadNamespace");
    boost::optional<std::string> resource = catalog.lookupResourceName(firstResourceId);
    ASSERT_EQ(firstCollection, *resource);

    catalog.removeResource(firstResourceId, firstCollection);
    catalog.removeResource(firstResourceId, firstCollection);
    catalog.removeResource(thirdResourceId, thirdCollection);

    resource = catalog.lookupResourceName(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.lookupResourceName(thirdResourceId);
    ASSERT_EQ(boost::none, resource);
}

TEST_F(CollectionCatalogResourceMapTest, CollisionTest) {
    // firstCollection and secondCollection map to the same ResourceId.
    catalog.addResource(firstResourceId, firstCollection);
    catalog.addResource(secondResourceId, secondCollection);

    // Looking up the namespace on a ResourceId while it has a collision should
    // return the empty string.
    boost::optional<std::string> resource = catalog.lookupResourceName(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.lookupResourceName(secondResourceId);
    ASSERT_EQ(boost::none, resource);

    // We remove a namespace, resolving the collision.
    catalog.removeResource(firstResourceId, firstCollection);
    resource = catalog.lookupResourceName(secondResourceId);
    ASSERT_EQ(secondCollection, *resource);

    // Adding the same namespace twice does not create a collision.
    catalog.addResource(secondResourceId, secondCollection);
    resource = catalog.lookupResourceName(secondResourceId);
    ASSERT_EQ(secondCollection, *resource);

    // The map should function normally for entries without collisions.
    catalog.addResource(firstResourceId, firstCollection);
    resource = catalog.lookupResourceName(secondResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.addResource(thirdResourceId, thirdCollection);
    resource = catalog.lookupResourceName(thirdResourceId);
    ASSERT_EQ(thirdCollection, *resource);

    catalog.removeResource(thirdResourceId, thirdCollection);
    resource = catalog.lookupResourceName(thirdResourceId);
    ASSERT_EQ(boost::none, resource);

    catalog.removeResource(firstResourceId, firstCollection);
    catalog.removeResource(secondResourceId, secondCollection);

    resource = catalog.lookupResourceName(firstResourceId);
    ASSERT_EQ(boost::none, resource);

    resource = catalog.lookupResourceName(secondResourceId);
    ASSERT_EQ(boost::none, resource);
}

class CollectionCatalogResourceTest : public unittest::Test {
public:
    void setUp() {
        for (int i = 0; i < 5; i++) {
            NamespaceString nss("resourceDb", "coll" + std::to_string(i));
            std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
            auto uuid = collection->uuid();

            catalog.registerCollection(&opCtx, uuid, std::move(collection));
        }

        int numEntries = 0;
        for (auto it = catalog.begin(&opCtx, DatabaseName(boost::none, "resourceDb"));
             it != catalog.end(&opCtx);
             it++) {
            auto coll = *it;
            std::string collName = coll->ns().ns();
            ResourceId rid(RESOURCE_COLLECTION, collName);

            ASSERT_NE(catalog.lookupResourceName(rid), boost::none);
            numEntries++;
        }
        ASSERT_EQ(5, numEntries);
    }

    void tearDown() {
        std::vector<UUID> collectionsToDeregister;
        for (auto it = catalog.begin(&opCtx, DatabaseName(boost::none, "resourceDb"));
             it != catalog.end(&opCtx);
             ++it) {
            auto coll = *it;
            auto uuid = coll->uuid();
            if (!coll) {
                break;
            }

            collectionsToDeregister.push_back(uuid);
        }

        for (auto&& uuid : collectionsToDeregister) {
            catalog.deregisterCollection(&opCtx, uuid);
        }

        int numEntries = 0;
        for (auto it = catalog.begin(&opCtx, DatabaseName(boost::none, "resourceDb"));
             it != catalog.end(&opCtx);
             it++) {
            numEntries++;
        }
        ASSERT_EQ(0, numEntries);
    }

protected:
    OperationContextNoop opCtx;
    CollectionCatalog catalog;
};

TEST_F(CollectionCatalogResourceTest, RemoveAllResources) {
    catalog.deregisterAllCollectionsAndViews();

    const std::string dbName = "resourceDb";
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    ASSERT_EQ(boost::none, catalog.lookupResourceName(rid));

    for (int i = 0; i < 5; i++) {
        NamespaceString nss("resourceDb", "coll" + std::to_string(i));
        rid = ResourceId(RESOURCE_COLLECTION, nss.ns());
        ASSERT_EQ(boost::none, catalog.lookupResourceName((rid)));
    }
}

TEST_F(CollectionCatalogResourceTest, LookupDatabaseResource) {
    const std::string dbName = "resourceDb";
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    boost::optional<std::string> ridStr = catalog.lookupResourceName(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(dbName) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingDatabaseResource) {
    const std::string dbName = "missingDb";
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    ASSERT(!catalog.lookupResourceName(rid));
}

TEST_F(CollectionCatalogResourceTest, LookupCollectionResource) {
    const std::string collNs = "resourceDb.coll1";
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    boost::optional<std::string> ridStr = catalog.lookupResourceName(rid);

    ASSERT(ridStr);
    ASSERT(ridStr->find(collNs) != std::string::npos);
}

TEST_F(CollectionCatalogResourceTest, LookupMissingCollectionResource) {
    const std::string dbName = "resourceDb.coll5";
    auto rid = ResourceId(RESOURCE_COLLECTION, dbName);
    ASSERT(!catalog.lookupResourceName(rid));
}

TEST_F(CollectionCatalogResourceTest, RemoveCollection) {
    const std::string collNs = "resourceDb.coll1";
    auto coll = catalog.lookupCollectionByNamespace(&opCtx, NamespaceString(collNs));
    catalog.deregisterCollection(&opCtx, coll->uuid());
    auto rid = ResourceId(RESOURCE_COLLECTION, collNs);
    ASSERT(!catalog.lookupResourceName(rid));
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
    auto it = catalog.begin(&opCtx, DatabaseName(boost::none, "bar"));
    auto collsIt = collsIterator("bar");
    auto uuid = collsIt->first;
    catalog.deregisterCollection(&opCtx, uuid);
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
    catalog.registerCollection(opCtx.get(), newUUID, std::move(newCollShared));
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

    catalog.deregisterCollection(opCtx.get(), colUUID);
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
    catalog.registerCollection(opCtx.get(), uuid, std::move(collShared));
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
        catalog.onCloseCatalog(opCtx.get());
    }

    catalog.deregisterCollection(opCtx.get(), colUUID);
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);

    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onOpenCatalog(opCtx.get());
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
        catalog.onCloseCatalog(opCtx.get());
    }

    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), newUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(opCtx.get(), newUUID), boost::none);
    catalog.registerCollection(opCtx.get(), newUUID, std::move(newCollShared));
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), newUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);

    // Ensure that collection still exists after opening the catalog again.
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onOpenCatalog(opCtx.get());
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
        catalog.onCloseCatalog(opCtx.get());
    }

    catalog.deregisterCollection(opCtx.get(), colUUID);
    ASSERT(catalog.lookupCollectionByUUID(opCtx.get(), colUUID) == nullptr);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), nss);
    catalog.registerCollection(opCtx.get(), colUUID, std::move(newCollShared));
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), colUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), newNss);

    // Ensure that collection still exists after opening the catalog again.
    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onOpenCatalog(opCtx.get());
    }

    ASSERT_EQUALS(catalog.lookupCollectionByUUID(opCtx.get(), colUUID), newCol);
    ASSERT_EQUALS(*catalog.lookupNSSByUUID(opCtx.get(), colUUID), newNss);
}

// Re-opening the catalog should increment the CollectionCatalog's epoch.
TEST_F(CollectionCatalogTest, CollectionCatalogEpoch) {
    auto originalEpoch = catalog.getEpoch();

    {
        Lock::GlobalLock globalLk(opCtx.get(), MODE_X);
        catalog.onCloseCatalog(opCtx.get());
        catalog.onOpenCatalog(opCtx.get());
    }

    auto incrementedEpoch = catalog.getEpoch();
    ASSERT_EQ(originalEpoch + 1, incrementedEpoch);
}

DEATH_TEST_F(CollectionCatalogResourceTest, AddInvalidResourceType, "invariant") {
    auto rid = ResourceId(RESOURCE_GLOBAL, 0);
    catalog.addResource(rid, "");
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
        catalog.registerCollection(opCtx.get(), uuid, std::move(newColl));
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

    catalog.deregisterAllCollectionsAndViews();
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
        catalog.registerCollection(opCtx.get(), uuid, std::move(newColl));
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

    catalog.deregisterAllCollectionsAndViews();
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

}  // namespace
}  // namespace mongo
