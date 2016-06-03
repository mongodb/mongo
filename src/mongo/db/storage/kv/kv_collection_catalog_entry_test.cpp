/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <iostream>
#include <string>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

class KVCollectionCatalogEntryTest : public unittest::Test {
public:
    KVCollectionCatalogEntryTest()
        : _nss("unittests.kv_collection_catalog_entry"), _storageEngine(new DevNullKVEngine()) {
        _storageEngine.finishInit();
    }

    ~KVCollectionCatalogEntryTest() {
        _storageEngine.cleanShutdown();
    }

    std::unique_ptr<OperationContext> newOperationContext() {
        return stdx::make_unique<OperationContextNoop>(_storageEngine.newRecoveryUnit());
    }

    void setUp() final {
        auto opCtx = newOperationContext();
        DatabaseCatalogEntry* dbEntry =
            _storageEngine.getDatabaseCatalogEntry(opCtx.get(), _nss.db());

        {
            WriteUnitOfWork wuow(opCtx.get());
            const bool allocateDefaultSpace = true;
            ASSERT_OK(dbEntry->createCollection(
                opCtx.get(), _nss.ns(), CollectionOptions(), allocateDefaultSpace));
            wuow.commit();
        }
    }

    CollectionCatalogEntry* getCollectionCatalogEntry() {
        auto opCtx = newOperationContext();
        DatabaseCatalogEntry* dbEntry =
            _storageEngine.getDatabaseCatalogEntry(opCtx.get(), _nss.db());
        return dbEntry->getCollectionCatalogEntry(_nss.ns());
    }

    std::string createIndex(BSONObj keyPattern, std::string indexType = IndexNames::BTREE) {
        auto opCtx = newOperationContext();
        std::string indexName = "idx" + std::to_string(numIndexesCreated);

        IndexDescriptor desc(
            nullptr,
            indexType,
            BSON("v" << 1 << "key" << keyPattern << "name" << indexName << "ns" << _nss.ns()));

        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(getCollectionCatalogEntry()->prepareForIndexBuild(opCtx.get(), &desc));
            wuow.commit();
        }

        ++numIndexesCreated;
        return indexName;
    }

    void assertMultikeyPathsAreEqual(const MultikeyPaths& actual, const MultikeyPaths& expected) {
        bool match = (expected == actual);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expected) << ", "
                               << "Actual: "
                               << dumpMultikeyPaths(actual));
        }
        ASSERT(match);
    }

private:
    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }

    const NamespaceString _nss;
    KVStorageEngine _storageEngine;
    size_t numIndexesCreated = 0;
};

TEST_F(KVCollectionCatalogEntryTest, MultikeyPathsForBtreeIndexInitializedToVectorOfEmptySets) {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {std::set<size_t>{}, std::set<size_t>{}});
    }
}

TEST_F(KVCollectionCatalogEntryTest, CanSetIndividualPathComponentOfBtreeIndexAsMultikey) {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {std::set<size_t>{}, {0U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {std::set<size_t>{}, {0U}});
    }
}

TEST_F(KVCollectionCatalogEntryTest, MultikeyPathsAccumulateOnDifferentFields) {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {std::set<size_t>{}, {0U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {std::set<size_t>{}, {0U}});
    }

    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{0U}, std::set<size_t>{}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}, {0U}});
    }
}

TEST_F(KVCollectionCatalogEntryTest, MultikeyPathsAccumulateOnDifferentComponentsOfTheSameField) {
    std::string indexName = createIndex(BSON("a.b" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{0U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{1U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}});
    }
}

TEST_F(KVCollectionCatalogEntryTest, NoOpWhenSpecifiedPathComponentsAlreadySetAsMultikey) {
    std::string indexName = createIndex(BSON("a" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{0U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    ASSERT(!collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{0U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }
}

TEST_F(KVCollectionCatalogEntryTest, CanSetMultipleFieldsAndComponentsAsMultikey) {
    std::string indexName = createIndex(BSON("a.b.c" << 1 << "a.b.d" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{0U, 1U}, {0U, 1U}}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}, {0U, 1U}});
    }
}

DEATH_TEST_F(KVCollectionCatalogEntryTest,
             CannotOmitPathLevelMultikeyInfoWithBtreeIndex,
             "Invariant failure !multikeyPaths.empty()") {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    collEntry->setIndexIsMultikey(opCtx.get(), indexName, MultikeyPaths{});
}

DEATH_TEST_F(KVCollectionCatalogEntryTest,
             AtLeastOnePathComponentMustCauseIndexToBeMultikey,
             "Invariant failure somePathIsMultikey") {
    std::string indexName = createIndex(BSON("a" << 1 << "b" << 1));
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    collEntry->setIndexIsMultikey(opCtx.get(), indexName, {std::set<size_t>{}, std::set<size_t>{}});
}

TEST_F(KVCollectionCatalogEntryTest, PathLevelMultikeyTrackingIsSupportedBy2dsphereIndexes) {
    std::string indexType = IndexNames::GEO_2DSPHERE;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {std::set<size_t>{}, std::set<size_t>{}});
    }
}

TEST_F(KVCollectionCatalogEntryTest, PathLevelMultikeyTrackingIsNotSupportedByAllIndexTypes) {
    std::string indexTypes[] = {
        IndexNames::GEO_2D, IndexNames::GEO_HAYSTACK, IndexNames::TEXT, IndexNames::HASHED};

    for (auto&& indexType : indexTypes) {
        std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
        CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

        auto opCtx = newOperationContext();
        {
            MultikeyPaths multikeyPaths;
            ASSERT(!collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
            ASSERT(multikeyPaths.empty());
        }
    }
}

TEST_F(KVCollectionCatalogEntryTest, CanSetEntireTextIndexAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, MultikeyPaths{}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

TEST_F(KVCollectionCatalogEntryTest, NoOpWhenEntireIndexAlreadySetAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    ASSERT(collEntry->setIndexIsMultikey(opCtx.get(), indexName, MultikeyPaths{}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }

    ASSERT(!collEntry->setIndexIsMultikey(opCtx.get(), indexName, MultikeyPaths{}));

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collEntry->isIndexMultikey(opCtx.get(), indexName, &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

DEATH_TEST_F(KVCollectionCatalogEntryTest,
             CannotSetIndividualPathComponentsOfTextIndexAsMultikey,
             "Invariant failure multikeyPaths.empty()") {
    std::string indexType = IndexNames::TEXT;
    std::string indexName = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    CollectionCatalogEntry* collEntry = getCollectionCatalogEntry();

    auto opCtx = newOperationContext();
    collEntry->setIndexIsMultikey(opCtx.get(), indexName, {{0U}, {0U}});
}

}  // namespace
}  // namespace mongo
