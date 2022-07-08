/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/index_builds_coordinator.h"

#include "mongo/db/catalog/catalog_test_fixture.h"

namespace mongo {

namespace {

class IndexBuildsCoordinatorTest : public CatalogTestFixture {
public:
    /**
     * Creates collection 'nss' and inserts some documents with duplicate keys. It will possess a
     * default _id index.
     */
    void createCollectionWithDuplicateDocs(OperationContext* opCtx, const NamespaceString& nss);
};

void IndexBuildsCoordinatorTest::createCollectionWithDuplicateDocs(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    // Create collection.
    CollectionOptions options;
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, options));

    AutoGetCollection collection(opCtx, nss, MODE_X);
    invariant(collection);

    // Insert some data.
    OpDebug* const nullOpDebug = nullptr;
    for (int i = 0; i < 10; i++) {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection->insertDocument(
            opCtx, InsertStatement(BSON("_id" << i << "a" << 1)), nullOpDebug));
        wuow.commit();
    }

    ASSERT_EQ(collection->getIndexCatalog()->numIndexesTotal(opCtx), 1);
}

// Helper to refetch the Collection from the catalog in order to see any changes made to it
CollectionPtr coll(OperationContext* opCtx, const NamespaceString& nss) {
    return CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundUniqueEnforce) {
    auto opCtx = operationContext();
    NamespaceString nss("IndexBuildsCoordinatorTest.ForegroundUniqueEnforce");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec =
        BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    ASSERT_THROWS_CODE(indexBuildsCoord->createIndex(
                           opCtx, collection->uuid(), spec, indexConstraints, fromMigrate),
                       AssertionException,
                       ErrorCodes::DuplicateKey);
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 1);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundUniqueRelax) {
    auto opCtx = operationContext();
    NamespaceString nss("IndexBuildsCoordinatorTest.ForegroundUniqueRelax");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec =
        BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    auto fromMigrate = false;
    ASSERT_DOES_NOT_THROW(indexBuildsCoord->createIndex(
        opCtx, collection->uuid(), spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexAlreadyExists) {
    auto opCtx = operationContext();
    NamespaceString nss("IndexBuildsCoordinatorTest.ForegroundIndexAlreadyExists");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                         << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    auto uuid = collection->uuid();
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);

    // Should silently return if the index already exists.
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexOptionsConflictEnforce) {
    auto opCtx = operationContext();
    NamespaceString nss("IndexBuildsCoordinatorTest.ForegroundIndexOptionsConflictEnforce");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec1 = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey
                          << "name" << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto spec2 = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey
                          << "name" << (indexKey.firstElementFieldNameStringData() + "_2"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    auto uuid = collection->uuid();
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec1, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);

    ASSERT_THROWS_CODE(
        indexBuildsCoord->createIndex(opCtx, uuid, spec2, indexConstraints, fromMigrate),
        AssertionException,
        ErrorCodes::IndexOptionsConflict);
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexOptionsConflictRelax) {
    auto opCtx = operationContext();
    NamespaceString nss("IndexBuildsCoordinatorTest.ForegroundIndexOptionsConflictRelax");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec1 = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey
                          << "name" << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto spec2 = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey
                          << "name" << (indexKey.firstElementFieldNameStringData() + "_2"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    auto fromMigrate = false;
    auto uuid = collection->uuid();
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec1, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);

    // Should silently return in relax mode even if there are index option conflicts.
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec2, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(opCtx), 2);
}

}  // namespace
}  // namespace mongo
