/**
 *    Copyright (C) 2017 MongoDB Inc.
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
          col(stdx::make_unique<CollectionMock>(nss)),
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

        // Register dummy collection in catalog.
        catalog.onCreateCollection(&opCtx, &col, colUUID);
    }

protected:
    UUIDCatalog catalog;
    OperationContextNoop opCtx;
    NamespaceString nss;
    Collection col;
    CollectionUUID colUUID;
    CollectionUUID nextUUID;
    CollectionUUID prevUUID;
};

namespace {

TEST_F(UUIDCatalogTest, OnCreateCollection) {
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == &col);
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
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    ASSERT(catalog.lookupCollectionByUUID(newUUID) == nullptr);
    ASSERT(catalog.lookupNSSByUUID(newUUID) == NamespaceString());
    catalog.onCreateCollection(&opCtx, &newCol, newUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), &newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
}

TEST_F(UUIDCatalogTest, OnDropCollection) {
    catalog.onDropCollection(&opCtx, colUUID);
    // Ensure the lookup returns a null pointer upon removing the colUUID entry.
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == nullptr);
}

TEST_F(UUIDCatalogTest, OnRenameCollection) {
    auto oldUUID = CollectionUUID::gen();
    NamespaceString oldNss(nss.db(), "oldcol");
    Collection oldCol(stdx::make_unique<CollectionMock>(oldNss));
    catalog.onCreateCollection(&opCtx, &oldCol, oldUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(oldUUID), &oldCol);

    NamespaceString newNss(nss.db(), "newcol");
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));
    catalog.onRenameCollection(&opCtx, &newCol, oldUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(oldUUID), &newCol);
}

TEST_F(UUIDCatalogTest, NonExistingNextCol) {
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));
    ASSERT_FALSE(catalog.next(nss.db(), nextUUID));

    NamespaceString newNss("anotherdb", "newcol");
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));
    catalog.onCreateCollection(&opCtx, &newCol, nextUUID);
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));

    NamespaceString prevNss(nss.db(), "prevcol");
    Collection prevCol(stdx::make_unique<CollectionMock>(prevNss));
    catalog.onCreateCollection(&opCtx, &prevCol, prevUUID);
    ASSERT_FALSE(catalog.next(nss.db(), colUUID));
}

TEST_F(UUIDCatalogTest, ExistingNextCol) {
    NamespaceString nextNss(nss.db(), "next");
    Collection nextCol(stdx::make_unique<CollectionMock>(nextNss));
    catalog.onCreateCollection(&opCtx, &nextCol, nextUUID);
    auto next = catalog.next(nss.db(), colUUID);
    ASSERT_TRUE(next);
    ASSERT_EQUALS(*next, nextUUID);
}

TEST_F(UUIDCatalogTest, NonExistingPrevCol) {
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));
    ASSERT_FALSE(catalog.prev(nss.db(), prevUUID));

    NamespaceString newNss("anotherdb", "newcol");
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));
    catalog.onCreateCollection(&opCtx, &newCol, nextUUID);
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));

    NamespaceString nextNss(nss.db(), "nextcol");
    Collection nextCol(stdx::make_unique<CollectionMock>(nextNss));
    catalog.onCreateCollection(&opCtx, &nextCol, nextUUID);
    ASSERT_FALSE(catalog.prev(nss.db(), colUUID));
}

TEST_F(UUIDCatalogTest, ExistingPrevCol) {
    NamespaceString prevNss(nss.db(), "prevcol");
    Collection prevCol(stdx::make_unique<CollectionMock>(prevNss));
    catalog.onCreateCollection(&opCtx, &prevCol, prevUUID);
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
    Collection prevCol(stdx::make_unique<CollectionMock>(prevNss));
    catalog.onCreateCollection(&opCtx, &prevCol, prevUUID);

    NamespaceString nextNss(nss.db(), "nextcol");
    Collection nextCol(stdx::make_unique<CollectionMock>(nextNss));
    catalog.onCreateCollection(&opCtx, &nextCol, nextUUID);

    catalog.onDropCollection(&opCtx, colUUID);

    auto nextPrev = catalog.prev(nss.db(), nextUUID);
    ASSERT(nextPrev);
    ASSERT_EQUALS(*nextPrev, prevUUID);

    auto prevNext = catalog.next(nss.db(), prevUUID);
    ASSERT(prevNext);
    ASSERT_EQUALS(*prevNext, nextUUID);
}

TEST_F(UUIDCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsOldNSSIfDropped) {
    catalog.onCloseCatalog();
    catalog.onDropCollection(&opCtx, colUUID);
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
    catalog.onOpenCatalog();
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), NamespaceString());
}

TEST_F(UUIDCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsNewlyCreatedNSS) {
    auto newUUID = CollectionUUID::gen();
    NamespaceString newNss(nss.db(), "newcol");
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    catalog.onCloseCatalog();
    ASSERT(catalog.lookupCollectionByUUID(newUUID) == nullptr);
    ASSERT(catalog.lookupNSSByUUID(newUUID) == NamespaceString());
    catalog.onCreateCollection(&opCtx, &newCol, newUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), &newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);

    // Ensure that collection still exists after opening the catalog again.
    catalog.onOpenCatalog();
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), &newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
}

TEST_F(UUIDCatalogTest, LookupNSSByUUIDForClosedCatalogReturnsFreshestNSS) {
    NamespaceString newNss(nss.db(), "newcol");
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));
    catalog.onCloseCatalog();
    catalog.onDropCollection(&opCtx, colUUID);
    ASSERT(catalog.lookupCollectionByUUID(colUUID) == nullptr);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), nss);
    catalog.onCreateCollection(&opCtx, &newCol, colUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(colUUID), &newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), newNss);

    // Ensure that collection still exists after opening the catalog again.
    catalog.onOpenCatalog();
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(colUUID), &newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(colUUID), newNss);
}
}  // namespace
