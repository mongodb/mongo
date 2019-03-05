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
#include "mongo/db/catalog/uuid_catalog.h"

#include <algorithm>

#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

/**
 * A test fixture that creates a UUID Catalog and Collection* pointer to store in it.
 */
class UUIDCatalogTest : public unittest::Test {
public:
    UUIDCatalogTest()
        : nss("testdb", "testcol"),
          col(nullptr),
          colUUID(CollectionUUID::gen()),
          nextUUID(CollectionUUID::gen()),
          prevUUID(CollectionUUID::gen()) {
        if (prevUUID > colUUID)
            std::swap(prevUUID, colUUID);
        if (colUUID > nextUUID)
            std::swap(colUUID, nextUUID);
        if (prevUUID > colUUID)
            std::swap(prevUUID, colUUID);
        ASSERT_GT(colUUID, prevUUID);
        ASSERT_GT(nextUUID, colUUID);

        auto collection = std::make_unique<CollectionMock>(nss);
        col = collection.get();
        // Register dummy collection in catalog.
        catalog.onCreateCollection(&opCtx, std::move(collection), colUUID);
    }

protected:
    UUIDCatalog catalog;
    OperationContextNoop opCtx;
    NamespaceString nss;
    CollectionMock* col;
    CollectionUUID colUUID;
    CollectionUUID nextUUID;
    CollectionUUID prevUUID;
};

class UUIDCatalogIterationTest : public unittest::Test {
public:
    void setUp() {
        for (int counter = 0; counter < 5; ++counter) {
            NamespaceString fooNss("foo", "coll" + std::to_string(counter));
            NamespaceString barNss("bar", "coll" + std::to_string(counter));

            auto fooUuid = CollectionUUID::gen();
            auto barUuid = CollectionUUID::gen();
            auto fooColl = std::make_unique<CollectionMock>(fooNss);
            auto barColl = std::make_unique<CollectionMock>(barNss);

            dbMap["foo"].insert(std::make_pair(fooUuid, fooColl.get()));
            dbMap["bar"].insert(std::make_pair(barUuid, barColl.get()));
            catalog.onCreateCollection(&opCtx, std::move(fooColl), fooUuid);
            catalog.onCreateCollection(&opCtx, std::move(barColl), barUuid);
        }
    }

    void tearDown() {
        for (auto& it : dbMap) {
            for (auto& kv : it.second) {
                catalog.onDropCollection(&opCtx, kv.first);
            }
        }
    }

    std::map<CollectionUUID, CollectionMock*>::iterator collsIterator(std::string dbName) {
        auto it = dbMap.find(dbName);
        ASSERT(it != dbMap.end());
        return it->second.begin();
    }

    std::map<CollectionUUID, CollectionMock*>::iterator collsIteratorEnd(std::string dbName) {
        auto it = dbMap.find(dbName);
        ASSERT(it != dbMap.end());
        return it->second.end();
    }

    void checkCollections(std::string dbName) {
        unsigned long counter = 0;

        for (auto[orderedIt, catalogIt] = std::tuple{collsIterator(dbName), catalog.begin(dbName)};
             catalogIt != catalog.end() && orderedIt != collsIteratorEnd(dbName);
             ++catalogIt, ++orderedIt) {

            auto catalogColl = *catalogIt;
            ASSERT(catalogColl != nullptr);
            auto orderedColl = orderedIt->second;
            ASSERT_EQ(catalogColl->ns(), orderedColl->ns());
            ++counter;
        }

        ASSERT_EQUALS(counter, dbMap[dbName].size());
    }

protected:
    UUIDCatalog catalog;
    OperationContextNoop opCtx;
    std::map<std::string, std::map<CollectionUUID, CollectionMock*>> dbMap;
};

namespace {

// Create an iterator over the UUIDCatalog and assert that all collections are present.
// Iteration ends when the end of the catalog is reached.
TEST_F(UUIDCatalogIterationTest, EndAtEndOfCatalog) {
    checkCollections("foo");
}

// Create an iterator over the UUIDCatalog and test that all collections are present. Iteration ends
// when the end of a database-specific section of the catalog is reached.
TEST_F(UUIDCatalogIterationTest, EndAtEndOfSection) {
    checkCollections("bar");
}

// Delete an entry in the catalog while iterating.
TEST_F(UUIDCatalogIterationTest, InvalidateEntry) {
    auto it = catalog.begin("bar");

    // Invalidate bar.coll1.
    for (auto collsIt = collsIterator("bar"); collsIt != collsIteratorEnd("bar"); ++collsIt) {
        if (collsIt->second->ns().ns() == "bar.coll1") {
            catalog.onDropCollection(&opCtx, collsIt->first);
            break;
        }
    }

    // Ensure bar.coll1 is not returned by the iterator.
    for (; it != catalog.end(); ++it) {
        auto coll = *it;
        ASSERT(coll && coll->ns().ns() != "bar.coll1");
    }
}

// Delete the entry pointed to by the iterator and dereference the iterator.
TEST_F(UUIDCatalogIterationTest, InvalidateAndDereference) {
    auto it = catalog.begin("bar");
    auto collsIt = collsIterator("bar");
    catalog.onDropCollection(&opCtx, collsIt->first);
    ++collsIt;

    auto catalogColl = *it;
    ASSERT(catalogColl != nullptr);
    ASSERT_EQUALS(catalogColl->ns(), collsIt->second->ns());
}

// Delete the last entry for a database while pointing to it and dereference the iterator.
TEST_F(UUIDCatalogIterationTest, InvalidateLastEntryAndDereference) {
    auto it = catalog.begin("bar");
    NamespaceString lastNs;
    boost::optional<CollectionUUID> uuid;
    for (auto collsIt = collsIterator("bar"); collsIt != collsIteratorEnd("bar"); ++collsIt) {
        lastNs = collsIt->second->ns();
        uuid = collsIt->first;
    }

    // Increment until it points to the last collection.
    for (; it != catalog.end(); ++it) {
        auto coll = *it;
        ASSERT(coll != nullptr);
        if (coll->ns() == lastNs) {
            break;
        }
    }

    catalog.onDropCollection(&opCtx, *uuid);
    ASSERT(*it == nullptr);
}

// Delete the last entry in the map while pointing to it and dereference the iterator.
TEST_F(UUIDCatalogIterationTest, InvalidateLastEntryInMapAndDereference) {
    auto it = catalog.begin("foo");
    NamespaceString lastNs;
    boost::optional<CollectionUUID> uuid;
    for (auto collsIt = collsIterator("foo"); collsIt != collsIteratorEnd("foo"); ++collsIt) {
        lastNs = collsIt->second->ns();
        uuid = collsIt->first;
    }

    // Increment until it points to the last collection.
    for (; it != catalog.end(); ++it) {
        auto coll = *it;
        ASSERT(coll != nullptr);
        if (coll->ns() == lastNs) {
            break;
        }
    }

    catalog.onDropCollection(&opCtx, *uuid);
    ASSERT(*it == nullptr);
}

TEST_F(UUIDCatalogTest, OnCreateCollection) {
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == col);
}

TEST_F(UUIDCatalogTest, LookupCollectionByUUID) {
    // Ensure the string value of the NamespaceString of the obtained Collection is equal to
    // nss.ns().
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(colUUID)->ns().ns(), nss.ns());
    // Ensure lookups of unknown UUIDs result in null pointers.
    ASSERT(catalog.lookupCollectionByUUID(CollectionUUID::gen()) == nullptr);
}

TEST_F(UUIDCatalogTest, LookupNSSByUUID) {
    // Ensure the string value of the obtained NamespaceString is equal to nss.ns().
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID).ns(), nss.ns());
    // Ensure namespace lookups of unknown UUIDs result in empty NamespaceStrings.
    ASSERT_EQUALS(catalog.lookupNSSByUUID(CollectionUUID::gen()).ns(), NamespaceString().ns());
}

TEST_F(UUIDCatalogTest, InsertAfterLookup) {
    auto newUUID = CollectionUUID::gen();
    NamespaceString newNss(nss.db(), "newcol");
    auto newCollUnique = std::make_unique<CollectionMock>(newNss);
    auto newCol = newCollUnique.get();

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    ASSERT(catalog.lookupCollectionByUUID(newUUID) == nullptr);
    ASSERT(catalog.lookupNSSByUUID(newUUID) == NamespaceString());
    catalog.onCreateCollection(&opCtx, std::move(newCollUnique), newUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
}

TEST_F(UUIDCatalogTest, OnDropCollection) {
    catalog.onDropCollection(&opCtx, colUUID);
    // Ensure the lookup returns a null pointer upon removing the colUUID entry.
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == nullptr);
}

TEST_F(UUIDCatalogTest, RenameCollection) {
    auto uuid = CollectionUUID::gen();
    NamespaceString oldNss(nss.db(), "oldcol");
    auto collUnique = std::make_unique<CollectionMock>(oldNss);
    auto collection = collUnique.get();
    catalog.onCreateCollection(&opCtx, std::move(collUnique), uuid);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(uuid), collection);

    NamespaceString newNss(nss.db(), "newcol");
    catalog.setCollectionNamespace(&opCtx, collection, oldNss, newNss);
    ASSERT_EQ(collection->ns(), newNss);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(uuid), collection);
}

TEST_F(UUIDCatalogTest, NonExistingNextCol) {
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));
    ASSERT_FALSE(catalog.next(nss.db(), nextUUID));

    NamespaceString newNss("anotherdb", "newcol");
    auto newColl = std::make_unique<CollectionMock>(newNss);
    catalog.onCreateCollection(&opCtx, std::move(newColl), nextUUID);
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));

    NamespaceString prevNss(nss.db(), "prevcol");
    auto prevColl = std::make_unique<CollectionMock>(prevNss);
    catalog.onCreateCollection(&opCtx, std::move(prevColl), prevUUID);
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));
}

TEST_F(UUIDCatalogTest, ExistingNextCol) {
    NamespaceString nextNss(nss.db(), "next");
    auto newColl = std::make_unique<CollectionMock>(nextNss);
    catalog.onCreateCollection(&opCtx, std::move(newColl), nextUUID);
    auto next = catalog.next(nss.db(), colUUID);
    ASSERT_TRUE(next);
    ASSERT_EQUALS(*next, nextUUID);
}

TEST_F(UUIDCatalogTest, NonExistingPrevCol) {
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));
    ASSERT_FALSE(catalog.prev(nss.db(), prevUUID));

    NamespaceString newNss("anotherdb", "newcol");
    auto newColl = std::make_unique<CollectionMock>(newNss);
    catalog.onCreateCollection(&opCtx, std::move(newColl), nextUUID);
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));

    NamespaceString nextNss(nss.db(), "nextcol");
    auto nextColl = std::make_unique<CollectionMock>(nextNss);
    catalog.onCreateCollection(&opCtx, std::move(nextColl), nextUUID);
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));
}

TEST_F(UUIDCatalogTest, ExistingPrevCol) {
    NamespaceString prevNss(nss.db(), "prevcol");
    auto prevColl = std::make_unique<CollectionMock>(prevNss);
    catalog.onCreateCollection(&opCtx, std::move(prevColl), prevUUID);
    auto prev = catalog.prev(nss.db(), colUUID);
    ASSERT_TRUE(prev);
    ASSERT_EQUALS(*prev, prevUUID);
}

TEST_F(UUIDCatalogTest, NextPrevColOnEmptyCatalog) {
    catalog.onDropCollection(&opCtx, colUUID);
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));
    ASSERT_FALSE(catalog.next(nss.db(), prevUUID));
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));
    ASSERT_FALSE(catalog.prev(nss.db(), nextUUID));
}

TEST_F(UUIDCatalogTest, InvalidateOrdering) {
    NamespaceString prevNss(nss.db(), "prevcol");
    auto prevColl = std::make_unique<CollectionMock>(prevNss);
    catalog.onCreateCollection(&opCtx, std::move(prevColl), prevUUID);

    NamespaceString nextNss(nss.db(), "nextcol");
    auto nextColl = std::make_unique<CollectionMock>(nextNss);
    catalog.onCreateCollection(&opCtx, std::move(nextColl), nextUUID);

    catalog.onDropCollection(&opCtx, colUUID);

    auto nextPrev = catalog.prev(nss.db(), nextUUID);
    ASSERT(nextPrev);
    ASSERT_EQUALS(*nextPrev, prevUUID);

    auto prevNext = catalog.next(nss.db(), prevUUID);
    ASSERT(prevNext);
    ASSERT_EQUALS(*prevNext, nextUUID);
}

TEST_F(UUIDCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsOldNSSIfDropped) {
    catalog.onCloseCatalog(&opCtx);
    catalog.onDropCollection(&opCtx, colUUID);
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
    catalog.onOpenCatalog(&opCtx);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), NamespaceString());
}

TEST_F(UUIDCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsNewlyCreatedNSS) {
    auto newUUID = CollectionUUID::gen();
    NamespaceString newNss(nss.db(), "newcol");
    auto newCollUnique = std::make_unique<CollectionMock>(newNss);
    auto newCol = newCollUnique.get();

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    catalog.onCloseCatalog(&opCtx);
    ASSERT(catalog.lookupCollectionByUUID(newUUID) == nullptr);
    ASSERT(catalog.lookupNSSByUUID(newUUID) == NamespaceString());
    catalog.onCreateCollection(&opCtx, std::move(newCollUnique), newUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);

    // Ensure that collection still exists after opening the catalog again.
    catalog.onOpenCatalog(&opCtx);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
}

TEST_F(UUIDCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsFreshestNSS) {
    NamespaceString newNss(nss.db(), "newcol");
    auto newCollUnique = std::make_unique<CollectionMock>(newNss);
    auto newCol = newCollUnique.get();

    catalog.onCloseCatalog(&opCtx);
    catalog.onDropCollection(&opCtx, colUUID);
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
    catalog.onCreateCollection(&opCtx, std::move(newCollUnique), colUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(colUUID), newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), newNss);

    // Ensure that collection still exists after opening the catalog again.
    catalog.onOpenCatalog(&opCtx);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(colUUID), newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), newNss);
}
}  // namespace
