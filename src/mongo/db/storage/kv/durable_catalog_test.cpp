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

#include <boost/optional/optional_io.hpp>
#include <iostream>
#include <string>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

static std::string kSideWritesTableIdent("sideWrites");
static std::string kConstraintViolationsTableIdent("constraintViolations");

// Update version as breaking changes are introduced into the index build procedure.
static const long kExpectedVersion = 1;

class DurableCatalogTest : public CatalogTestFixture {
public:
    void setUp() final {
        CatalogTestFixture::setUp();

        _nss = NamespaceString("unittests.durable_catalog");
        _catalogId = createCollection(_nss);
    }

    NamespaceString ns() {
        return _nss;
    }

    DurableCatalog* getCatalog() {
        return operationContext()->getServiceContext()->getStorageEngine()->getCatalog();
    }

    RecordId getCatalogId() {
        return _catalogId;
    }

    RecordId createCollection(const NamespaceString& nss) {
        Lock::DBLock dbLk(operationContext(), nss.db(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), nss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        const bool allocateDefaultSpace = true;
        CollectionOptions options;
        options.uuid = UUID::gen();

        auto swColl =
            getCatalog()->createCollection(operationContext(), nss, options, allocateDefaultSpace);
        ASSERT_OK(swColl.getStatus());

        std::pair<RecordId, std::unique_ptr<RecordStore>> coll = std::move(swColl.getValue());
        RecordId catalogId = coll.first;

        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss, catalogId);
        CollectionCatalog::write(operationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                operationContext(), options.uuid.get(), std::move(collection));
        });

        wuow.commit();

        return catalogId;
    }

    std::string createIndex(BSONObj keyPattern,
                            std::string indexType = IndexNames::BTREE,
                            bool twoPhase = false) {
        std::string indexName = "idx" + std::to_string(numIndexesCreated);

        auto collection = std::make_unique<CollectionMock>(_nss);
        IndexDescriptor desc(indexType,
                             BSON("v" << 1 << "key" << keyPattern << "name" << indexName));

        {
            WriteUnitOfWork wuow(operationContext());
            const bool isSecondaryBackgroundIndexBuild = false;
            boost::optional<UUID> buildUUID(twoPhase, UUID::gen());
            ASSERT_OK(getCatalog()->prepareForIndexBuild(
                operationContext(), _catalogId, &desc, buildUUID, isSecondaryBackgroundIndexBuild));
            wuow.commit();
        }

        ++numIndexesCreated;
        return indexName;
    }

    void assertMultikeyPathsAreEqual(const MultikeyPaths& actual, const MultikeyPaths& expected) {
        bool match = (expected == actual);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expected) << ", "
                               << "Actual: " << dumpMultikeyPaths(actual));
        }
        ASSERT(match);
    }

    StatusWith<DurableCatalog::ImportResult> importCollectionTest(const NamespaceString& nss,
                                                                  const BSONObj& metadata) {
        Lock::DBLock dbLock(operationContext(), nss.db(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), nss, MODE_X);

        WriteUnitOfWork wuow(operationContext());
        auto res = getCatalog()
                       ->importCollection(operationContext(),
                                          nss,
                                          metadata,
                                          BSON("storage"
                                               << "metadata"),
                                          DurableCatalog::ImportCollectionUUIDOption::kGenerateNew);
        if (res.isOK()) {
            wuow.commit();
        }
        return res;
    }

private:
    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto& multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto& multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }

    NamespaceString _nss;
    size_t numIndexesCreated = 0;
    RecordId _catalogId;
};

TEST_F(DurableCatalogTest, MultikeyPathsForBtreeIndexInitializedToVectorOfEmptySets) {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    DurableCatalog* catalog = getCatalog();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, MultikeyComponents{}});
    }
}

TEST_F(DurableCatalogTest, CanSetIndividualPathComponentOfBtreeIndexAsMultikey) {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, {MultikeyComponents{}, {0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, {0U}});
    }
}

TEST_F(DurableCatalogTest, MultikeyPathsAccumulateOnDifferentFields) {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, {MultikeyComponents{}, {0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, {0U}});
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, {{0U}, MultikeyComponents{}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}, {0U}});
    }
}

TEST_F(DurableCatalogTest, MultikeyPathsAccumulateOnDifferentComponentsOfTheSameField) {
    std::string indexName = createIndex(BSON("a.b" << 1));
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(operationContext(), getCatalogId(), indexName, {{0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(operationContext(), getCatalogId(), indexName, {{1U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}});
    }
}

TEST_F(DurableCatalogTest, NoOpWhenSpecifiedPathComponentsAlreadySetAsMultikey) {
    std::string indexName = createIndex(BSON("a" << 1));
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(operationContext(), getCatalogId(), indexName, {{0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(!catalog->setIndexIsMultikey(operationContext(), getCatalogId(), indexName, {{0U}}));
        // Rollback WUOW.
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }
}

TEST_F(DurableCatalogTest, CanSetMultipleFieldsAndComponentsAsMultikey) {
    std::string indexName = createIndex(BSON("a.b.c" << 1 << "a.b.d" << 1));
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, {{0U, 1U}, {0U, 1U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}, {0U, 1U}});
    }
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   CannotOmitPathLevelMultikeyInfoWithBtreeIndex,
                   R"#(Invariant failure.*!multikeyPaths.empty\(\))#") {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    DurableCatalog* catalog = getCatalog();

    WriteUnitOfWork wuow(operationContext());
    catalog->setIndexIsMultikey(operationContext(), getCatalogId(), indexName, MultikeyPaths{});
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   AtLeastOnePathComponentMustCauseIndexToBeMultikey,
                   R"#(Invariant failure.*somePathIsMultikey)#") {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    DurableCatalog* catalog = getCatalog();

    WriteUnitOfWork wuow(operationContext());
    catalog->setIndexIsMultikey(operationContext(),
                                getCatalogId(),
                                indexName,
                                {MultikeyComponents{}, MultikeyComponents{}});
}

TEST_F(DurableCatalogTest, PathLevelMultikeyTrackingIsSupportedBy2dsphereIndexes) {
    std::string indexType = IndexNames::GEO_2DSPHERE;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    DurableCatalog* catalog = getCatalog();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, MultikeyComponents{}});
    }
}

TEST_F(DurableCatalogTest, PathLevelMultikeyTrackingIsNotSupportedByAllIndexTypes) {
    std::string indexTypes[] = {
        IndexNames::GEO_2D, IndexNames::GEO_HAYSTACK, IndexNames::TEXT, IndexNames::HASHED};

    for (auto&& indexType : indexTypes) {
        std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
        DurableCatalog* catalog = getCatalog();
        {
            MultikeyPaths multikeyPaths;
            ASSERT(!catalog->isIndexMultikey(
                operationContext(), getCatalogId(), indexName, &multikeyPaths));
            ASSERT(multikeyPaths.empty());
        }
    }
}

TEST_F(DurableCatalogTest, CanSetEntireTextIndexAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, MultikeyPaths{}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

TEST_F(DurableCatalogTest, NoOpWhenEntireIndexAlreadySetAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    DurableCatalog* catalog = getCatalog();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, MultikeyPaths{}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(!catalog->setIndexIsMultikey(
            operationContext(), getCatalogId(), indexName, MultikeyPaths{}));
        // Rollback WUOW.
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(catalog->isIndexMultikey(
            operationContext(), getCatalogId(), indexName, &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

TEST_F(DurableCatalogTest, SinglePhaseIndexBuild) {
    std::string indexName = createIndex(BSON("a" << 1));
    DurableCatalog* catalog = getCatalog();

    ASSERT_FALSE(catalog->isIndexReady(operationContext(), getCatalogId(), indexName));
    ASSERT_FALSE(catalog->getIndexBuildUUID(operationContext(), getCatalogId(), indexName));

    {
        WriteUnitOfWork wuow(operationContext());
        catalog->indexBuildSuccess(operationContext(), getCatalogId(), indexName);
        wuow.commit();
    }

    ASSERT_TRUE(catalog->isIndexReady(operationContext(), getCatalogId(), indexName));
    ASSERT_FALSE(catalog->getIndexBuildUUID(operationContext(), getCatalogId(), indexName));
}

TEST_F(DurableCatalogTest, TwoPhaseIndexBuild) {
    bool twoPhase = true;
    std::string indexName = createIndex(BSON("a" << 1), IndexNames::BTREE, twoPhase);
    DurableCatalog* catalog = getCatalog();

    ASSERT_FALSE(catalog->isIndexReady(operationContext(), getCatalogId(), indexName));
    ASSERT_TRUE(catalog->getIndexBuildUUID(operationContext(), getCatalogId(), indexName));

    {
        WriteUnitOfWork wuow(operationContext());
        catalog->indexBuildSuccess(operationContext(), getCatalogId(), indexName);
        wuow.commit();
    }

    ASSERT_TRUE(catalog->isIndexReady(operationContext(), getCatalogId(), indexName));
    ASSERT_FALSE(catalog->getIndexBuildUUID(operationContext(), getCatalogId(), indexName));
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   CannotSetIndividualPathComponentsOfTextIndexAsMultikey,
                   R"#(Invariant failure.*multikeyPaths.empty\(\))#") {
    std::string indexType = IndexNames::TEXT;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    DurableCatalog* catalog = getCatalog();

    WriteUnitOfWork wuow(operationContext());
    catalog->setIndexIsMultikey(operationContext(), getCatalogId(), indexName, {{0U}, {0U}});
}

TEST_F(DurableCatalogTest, ImportCollection) {
    // Import should fail if the namespace already exists.
    ASSERT_THROWS_CODE(
        importCollectionTest(ns(), {}), AssertionException, ErrorCodes::NamespaceExists);

    const auto nss = NamespaceString("unittest.import");

    // Import should fail with empty metadata.
    ASSERT_THROWS_CODE(importCollectionTest(nss, {}), AssertionException, ErrorCodes::BadValue);

    BSONCollectionCatalogEntry::MetaData md;

    md.ns = nss.ns();

    CollectionOptions optionsWithUUID;
    optionsWithUUID.uuid = UUID::gen();
    md.options = optionsWithUUID;

    BSONCollectionCatalogEntry::IndexMetaData indexMetaData;
    indexMetaData.spec = BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                  << "_id_");
    indexMetaData.ready = true;
    md.indexes.push_back(indexMetaData);

    auto mdObj = md.toBSON();
    const auto ident = "collection-7-1792004489479993697";
    const auto idxIdent = "index-8-1792004489479993697";
    auto idxIdentObj = BSON("_id_" << idxIdent);

    // Import should fail with missing "md" field.
    ASSERT_THROWS_CODE(
        importCollectionTest(
            nss, BSON("idxIdent" << idxIdentObj << "ns" << nss.ns() << "ident" << ident)),
        AssertionException,
        ErrorCodes::BadValue);

    // Import should fail with missing "idxIdent" field.
    ASSERT_THROWS_CODE(
        importCollectionTest(nss, BSON("md" << mdObj << "ns" << nss.ns() << "ident" << ident)),
        AssertionException,
        ErrorCodes::BadValue);

    // Import should fail with missing "ident" field.
    ASSERT_THROWS_CODE(
        importCollectionTest(nss,
                             BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns" << nss.ns())),
        AssertionException,
        ErrorCodes::BadValue);

    // Import should success with validate inputs.
    auto swImportResult = importCollectionTest(
        nss,
        BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns" << nss.ns() << "ident" << ident));
    ASSERT_OK(swImportResult.getStatus());
    DurableCatalog::ImportResult importResult = std::move(swImportResult.getValue());

    // Validate the catalog entry for the imported collection.
    auto entry = getCatalog()->getEntry(importResult.catalogId);
    ASSERT_EQ(entry.nss, nss);
    ASSERT_EQ(entry.ident, ident);
    ASSERT_EQ(getCatalog()->getIndexIdent(operationContext(), importResult.catalogId, "_id_"),
              idxIdent);

    // Test that a collection UUID is generated for import.
    ASSERT_NE(optionsWithUUID.uuid.get(), importResult.uuid);
    // Substitute in the generated UUID and check that the rest of fields in the catalog entry
    // match.
    md.options.uuid = importResult.uuid;
    ASSERT_BSONOBJ_EQ(getCatalog()->getCatalogEntry(operationContext(), importResult.catalogId),
                      BSON("md" << md.toBSON() << "idxIdent" << idxIdentObj << "ns" << nss.ns()
                                << "ident" << ident));
}

TEST_F(DurableCatalogTest, IdentSuffixUsesRand) {
    const std::string rand = "0000000000000000000";
    getCatalog()->setRand_forTest(rand);

    const NamespaceString nss = NamespaceString("a.b");

    RecordId catalogId = createCollection(nss);
    ASSERT(StringData(getCatalog()->getEntry(catalogId).ident).endsWith(rand));
    ASSERT_EQUALS(getCatalog()->getRand_forTest(), rand);
}

TEST_F(DurableCatalogTest, ImportCollectionRandConflict) {
    const std::string rand = "0000000000000000000";
    getCatalog()->setRand_forTest(rand);

    {
        // Import a collection with the 'rand' suffix as part of the ident. This will force 'rand'
        // to be changed in the durable catalog internals.
        const auto nss = NamespaceString("unittest.import");
        BSONCollectionCatalogEntry::MetaData md;
        md.ns = nss.ns();

        CollectionOptions optionsWithUUID;
        optionsWithUUID.uuid = UUID::gen();
        md.options = optionsWithUUID;

        BSONCollectionCatalogEntry::IndexMetaData indexMetaData;
        indexMetaData.spec = BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                      << "_id_");
        indexMetaData.ready = true;
        md.indexes.push_back(indexMetaData);

        auto mdObj = md.toBSON();
        const auto ident = "collection-0-" + rand;
        const auto idxIdent = "index-0-" + rand;
        auto idxIdentObj = BSON("_id_" << idxIdent);

        auto swImportResult =
            importCollectionTest(nss,
                                 BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns" << nss.ns()
                                           << "ident" << ident));
        ASSERT_OK(swImportResult.getStatus());
    }

    ASSERT_NOT_EQUALS(getCatalog()->getRand_forTest(), rand);

    {
        // Check that a newly created collection doesn't use 'rand' as the suffix in the ident.
        const NamespaceString nss = NamespaceString("a.b");

        RecordId catalogId = createCollection(nss);
        ASSERT(!StringData(getCatalog()->getEntry(catalogId).ident).endsWith(rand));
    }

    ASSERT_NOT_EQUALS(getCatalog()->getRand_forTest(), rand);
}

}  // namespace
}  // namespace mongo
