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

#include "mongo/util/uuid_catalog.h"

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
        : uuid(CollectionUUID::gen()),
          nss("testdb", "testcol"),
          col(stdx::make_unique<CollectionMock>(nss)) {
        // Register dummy collection in catalog.
        catalog.onCreateCollection(&opCtx, &col, uuid);
    }

protected:
    UUIDCatalog catalog;
    OperationContextNoop opCtx;
    CollectionUUID uuid;
    NamespaceString nss;
    Collection col;
};

namespace {

TEST_F(UUIDCatalogTest, onCreateCollection) {
    ASSERT(catalog.lookupCollectionByUUID(uuid) == &col);
}

TEST_F(UUIDCatalogTest, lookupCollectionByUUID) {
    // Ensure the string value of the NamespaceString of the obtained Collection is equal to
    // nss.ns().
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(uuid)->ns().ns(), nss.ns());
    // Ensure lookups of unknown UUIDs result in null pointers.
    ASSERT(catalog.lookupCollectionByUUID(CollectionUUID::gen()) == nullptr);
}

TEST_F(UUIDCatalogTest, lookupNSSByUUID) {
    // Ensure the string value of the obtained NamespaceString is equal to nss.ns().
    ASSERT_EQUALS(catalog.lookupNSSByUUID(uuid).ns(), nss.ns());
    // Ensure namespace lookups of unknown UUIDs result in empty NamespaceStrings.
    ASSERT_EQUALS(catalog.lookupNSSByUUID(CollectionUUID::gen()).ns(), NamespaceString().ns());
}

TEST_F(UUIDCatalogTest, insertAfterLookup) {
    auto newUUID = CollectionUUID::gen();
    NamespaceString newNss(nss.db(), "newcol");
    Collection newCol(stdx::make_unique<CollectionMock>(newNss));

    // Ensure that looking up non-existing UUIDs doesn't affect later registration of those UUIDs.
    ASSERT(catalog.lookupCollectionByUUID(newUUID) == nullptr);
    ASSERT(catalog.lookupNSSByUUID(newUUID) == NamespaceString());
    catalog.onCreateCollection(&opCtx, &newCol, newUUID);
    ASSERT_EQUALS(catalog.lookupCollectionByUUID(newUUID), &newCol);
    ASSERT_EQUALS(catalog.lookupNSSByUUID(uuid), nss);
}

TEST_F(UUIDCatalogTest, onDropCollection) {
    catalog.onDropCollection(&opCtx, uuid);
    // Ensure the lookup returns a null pointer upon removing the uuid entry.
    ASSERT(catalog.lookupCollectionByUUID(uuid) == nullptr);
}
}  // namespace
