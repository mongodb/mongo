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
#include "mongo/db/local_catalog/durable_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
CollectionAcquisition acquireCollectionForWrite(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
}
CollectionAcquisition acquireCollectionForRead(OperationContext* opCtx,
                                               const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
        MODE_IS);
}


class DurableCatalogTest : public CatalogTestFixture {
public:
    explicit DurableCatalogTest(Options options = {}) : CatalogTestFixture(std::move(options)) {}

    void setUp() override {
        CatalogTestFixture::setUp();

        _nss = NamespaceString::createNamespaceString_forTest("unittests.durable_catalog");
        _collectionUUID = createCollection(_nss, CollectionOptions()).uuid;
    }

    NamespaceString ns() {
        return _nss;
    }

    MDBCatalog* getMDBCatalog() {
        return operationContext()->getServiceContext()->getStorageEngine()->getMDBCatalog();
    }

    std::string generateNewCollectionIdent(const NamespaceString& nss) {
        auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
        return storageEngine->generateNewCollectionIdent(nss.dbName());
    }

    std::string generateNewIndexIdent(const NamespaceString& nss) {
        auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
        return storageEngine->generateNewIndexIdent(nss.dbName());
    }

    CollectionPtr getCollection() {
        // The lifetime of the collection returned by the lookup is guaranteed to be valid as
        // it's controlled by the test. The initialization is therefore safe.
        return CollectionPtr::CollectionPtr_UNSAFE(
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByUUID(operationContext(), *_collectionUUID));
    }

    CollectionWriter getCollectionWriter() {
        return CollectionWriter(operationContext(), *_collectionUUID);
    }

    struct CollectionCatalogIdAndUUID {
        RecordId catalogId;
        UUID uuid;
    };

    CollectionCatalogIdAndUUID createCollection(const NamespaceString& nss,
                                                CollectionOptions options) {
        Lock::GlobalWrite lk(operationContext());
        Lock::DBLock dbLk(operationContext(), nss.dbName(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), nss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        options.uuid = UUID::gen();
        const auto ident = generateNewCollectionIdent(nss);
        const auto catalogId = getMDBCatalog()->reserveCatalogId(operationContext());
        auto rs = unittest::assertGet(durable_catalog::createCollection(
            operationContext(), catalogId, nss, ident, options, getMDBCatalog()));
        std::shared_ptr<Collection> collection = std::make_shared<CollectionImpl>(
            operationContext(),
            nss,
            catalogId,
            durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, getMDBCatalog())
                ->metadata,
            std::move(rs));
        collection->init(operationContext());

        CollectionCatalog::write(operationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(operationContext(),
                                       std::move(collection),
                                       /*ts=*/boost::none);
        });

        wuow.commit();

        return CollectionCatalogIdAndUUID{catalogId, *options.uuid};
    }

    IndexCatalogEntry* createIndex(BSONObj keyPattern,
                                   std::string indexType = IndexNames::BTREE,
                                   bool twoPhase = false) {
        Lock::DBLock dbLk(operationContext(), _nss.dbName(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), _nss, MODE_X);

        std::string indexName = "idx" + std::to_string(_numIndexesCreated);
        // Make sure we have a valid IndexSpec for the type requested
        IndexSpec spec;
        spec.version(1).name(indexName).addKeys(keyPattern);
        if (indexType == IndexNames::TEXT) {
            spec.textWeights(BSON("a" << 1));
            spec.textIndexVersion(2);
            spec.textDefaultLanguage("swedish");
        }

        auto desc = IndexDescriptor(indexType, spec.toBSON());

        IndexCatalogEntry* entry = nullptr;
        auto collWriter = getCollectionWriter();
        {
            WriteUnitOfWork wuow(operationContext());
            boost::optional<UUID> buildUUID(twoPhase, UUID::gen());
            ASSERT_OK(collWriter.getWritableCollection(operationContext())
                          ->prepareForIndexBuild(
                              operationContext(), &desc, generateNewIndexIdent(_nss), buildUUID));
            entry = collWriter.getWritableCollection(operationContext())
                        ->getIndexCatalog()
                        ->getWritableEntryByName(
                            operationContext(), indexName, IndexCatalog::InclusionPolicy::kAll);
            ASSERT(entry);
            wuow.commit();
        }

        ++_numIndexesCreated;
        return entry;
    }

    void assertMultikeyPathsAreEqual(const MultikeyPaths& actual, const MultikeyPaths& expected) {
        bool match = (expected == actual);
        if (!match) {
            FAIL(std::string(str::stream() << "Expected: " << dumpMultikeyPaths(expected) << ", "
                                           << "Actual: " << dumpMultikeyPaths(actual)));
        }
        ASSERT(match);
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

    size_t _numIndexesCreated = 0;

    boost::optional<UUID> _collectionUUID;
};

class ImportCollectionTest : public DurableCatalogTest {
public:
    explicit ImportCollectionTest() : DurableCatalogTest(Options{}.inMemory(false)) {}

protected:
    void setUp() override {
        DurableCatalogTest::setUp();

        Lock::DBLock dbLock(operationContext(), nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), nss, MODE_IX);

        WriteUnitOfWork wuow{operationContext()};

        const auto generatedIdent = generateNewCollectionIdent(nss);
        auto mdbCatalog = getMDBCatalog();
        const auto catalogId = mdbCatalog->reserveCatalogId(operationContext());
        unittest::assertGet(durable_catalog::createCollection(
            operationContext(), catalogId, nss, generatedIdent, {}, mdbCatalog));
        ident = getMDBCatalog()->getEntry(catalogId).ident;
        ASSERT_EQ(generatedIdent, ident);

        IndexDescriptor descriptor{"",
                                   BSON(IndexDescriptor::kKeyPatternFieldName
                                        << BSON("_id" << 1) << IndexDescriptor::kIndexNameFieldName
                                        << IndexConstants::kIdIndexName
                                        << IndexDescriptor::kIndexVersionFieldName << 2)};

        idxIdent = generateNewIndexIdent(nss);
        durable_catalog::CatalogEntryMetaData::IndexMetaData imd;
        imd.spec = descriptor.infoObj();
        imd.ready = true;

        md = durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, mdbCatalog)
                 ->metadata;
        md->insertIndex(std::move(imd));
        durable_catalog::putMetaData(operationContext(),
                                     catalogId,
                                     *md,
                                     mdbCatalog,
                                     BSON(IndexConstants::kIdIndexName << idxIdent));

        ASSERT_OK(durable_catalog::createIndex(operationContext(),
                                               catalogId,
                                               nss,
                                               CollectionOptions{.uuid = UUID::gen()},
                                               descriptor.toIndexConfig(),
                                               idxIdent));

        wuow.commit();

        auto engine = operationContext()->getServiceContext()->getStorageEngine()->getEngine();
        engine->checkpoint();

        storageMetadata =
            BSON(ident << unittest::assertGet(engine->getStorageMetadata(ident)) << idxIdent
                       << unittest::assertGet(engine->getStorageMetadata(idxIdent)));

        engine->dropIdentForImport(
            *operationContext(), *shard_role_details::getRecoveryUnit(operationContext()), ident);
        engine->dropIdentForImport(*operationContext(),
                                   *shard_role_details::getRecoveryUnit(operationContext()),
                                   idxIdent);
    }

    StatusWith<durable_catalog::ImportResult> importCollectionTest(const NamespaceString& nss,
                                                                   const BSONObj& metadata,
                                                                   const BSONObj& storageMetadata) {
        Lock::DBLock dbLock(operationContext(), nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), nss, MODE_X);

        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "Collection already exists. NS: " << nss.toStringForErrorMsg(),
                !CollectionCatalog::get(operationContext())
                     ->lookupCollectionByNamespace(operationContext(), nss));

        WriteUnitOfWork wuow(operationContext());
        auto res = durable_catalog::importCollection(operationContext(),
                                                     nss,
                                                     metadata,
                                                     storageMetadata,
                                                     /*generateNewUUID*/ true,
                                                     getMDBCatalog());
        if (res.isOK()) {
            wuow.commit();
        }
        return res;
    }

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("unittest", "import");
    std::string ident;
    std::string idxIdent;
    std::shared_ptr<durable_catalog::CatalogEntryMetaData> md;
    BSONObj storageMetadata;
};

TEST_F(DurableCatalogTest, MultikeyPathsForBtreeIndexInitializedToVectorOfEmptySets) {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, MultikeyComponents{}});
    }
}

TEST_F(DurableCatalogTest, CanSetIndividualPathComponentOfBtreeIndexAsMultikey) {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(operationContext(),
                                              indexEntry->descriptor()->indexName(),
                                              {MultikeyComponents{}, {0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, {0U}});
    }
}

TEST_F(DurableCatalogTest, MultikeyPathsAccumulateOnDifferentFields) {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(operationContext(),
                                              indexEntry->descriptor()->indexName(),
                                              {MultikeyComponents{}, {0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, {0U}});
    }

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(operationContext(),
                                              indexEntry->descriptor()->indexName(),
                                              {{0U}, MultikeyComponents{}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}, {0U}});
    }
}

TEST_F(DurableCatalogTest, MultikeyPathsAccumulateOnDifferentComponentsOfTheSameField) {
    auto indexEntry = createIndex(BSON("a.b" << 1));
    auto collection = getCollection();

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{1U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}});
    }
}

TEST_F(DurableCatalogTest, NoOpWhenSpecifiedPathComponentsAlreadySetAsMultikey) {
    auto indexEntry = createIndex(BSON("a" << 1));
    auto collection = getCollection();

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(!collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U}}));
        // Rollback WUOW.
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }
}

TEST_F(DurableCatalogTest, CanSetMultipleFieldsAndComponentsAsMultikey) {
    auto indexEntry = createIndex(BSON("a.b.c" << 1 << "a.b.d" << 1));
    auto collection = getCollection();
    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U, 1U}, {0U, 1U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}, {0U, 1U}});
    }
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   CannotOmitPathLevelMultikeyInfoWithBtreeIndex,
                   R"#(Invariant failure.*!multikeyPaths.empty\(\))#") {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    Lock::GlobalLock globalLock{operationContext(), MODE_IX};
    WriteUnitOfWork wuow(operationContext());
    collection->setIndexIsMultikey(
        operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{});
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   AtLeastOnePathComponentMustCauseIndexToBeMultikey,
                   R"#(Invariant failure.*somePathIsMultikey)#") {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    Lock::GlobalLock globalLock{operationContext(), MODE_IX};
    WriteUnitOfWork wuow(operationContext());
    collection->setIndexIsMultikey(operationContext(),

                                   indexEntry->descriptor()->indexName(),
                                   {MultikeyComponents{}, MultikeyComponents{}});
}

TEST_F(DurableCatalogTest, PathLevelMultikeyTrackingIsSupportedBy2dsphereIndexes) {
    std::string indexType = IndexNames::GEO_2DSPHERE;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, MultikeyComponents{}});
    }
}

TEST_F(DurableCatalogTest, PathLevelMultikeyTrackingIsNotSupportedByAllIndexTypes) {
    std::string indexTypes[] = {IndexNames::GEO_2D, IndexNames::TEXT, IndexNames::HASHED};

    for (auto&& indexType : indexTypes) {
        auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
        auto collection = getCollection();
        {
            MultikeyPaths multikeyPaths;
            ASSERT(!collection->isIndexMultikey(
                operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
            ASSERT(multikeyPaths.empty());
        }
    }
}

TEST_F(DurableCatalogTest, CanSetEntireTextIndexAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

TEST_F(DurableCatalogTest, NoOpWhenEntireIndexAlreadySetAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }

    {
        Lock::GlobalLock globalLock{operationContext(), MODE_IX};
        WriteUnitOfWork wuow(operationContext());
        ASSERT(!collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{}));
        // Rollback WUOW.
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

class ConcurrentMultikeyTest : public DurableCatalogTest {
public:
    void testConcurrentMultikey(BSONObj keyPattern,
                                const MultikeyPaths& first,
                                const MultikeyPaths& second,
                                const MultikeyPaths& expected) {
        /*
         * This test verifies that we can set multikey on two threads concurrently with the
         * following interleaving that do not cause a WCE from the storage engine:
         *
         * T1: open storage snapshot
         *
         * T1: set multikey paths to {first}
         *
         * T1: commit
         *
         * T2: open storage snapshot
         *
         * T2: set multikey paths to {second}
         *
         * T2: commit
         *
         * T1: onCommit handler
         *
         * T2: onCommit handler
         *
         */

        auto indexEntry = createIndex(keyPattern);
        auto collection = getCollection();

        stdx::mutex mutex;
        stdx::condition_variable cv;
        int numMultikeyCalls = 0;

        // Start a thread that will set multikey paths to 'first'. It will commit the change to the
        // storage engine but block before running the onCommit handler that updates the in-memory
        // state in the Collection instance.
        stdx::thread t([svcCtx = getServiceContext(),
                        &collection,
                        &indexEntry,
                        &first,
                        &mutex,
                        &cv,
                        &numMultikeyCalls] {
            ThreadClient client(svcCtx->getService());
            auto opCtx = client->makeOperationContext();

            Lock::GlobalLock globalLock{opCtx.get(), MODE_IX};
            WriteUnitOfWork wuow(opCtx.get());

            // Register a onCommit that will block until the main thread has committed its multikey
            // write. This onCommit handler is registered before any writes and will thus be
            // performed first, blocking all other onCommit handlers.
            shard_role_details::getRecoveryUnit(opCtx.get())
                ->onCommit([&mutex, &cv, &numMultikeyCalls](OperationContext*,
                                                            boost::optional<Timestamp> commitTime) {
                    stdx::unique_lock lock(mutex);

                    // Let the main thread now we have committed to the storage engine
                    numMultikeyCalls = 1;
                    cv.notify_all();

                    // Wait until the main thread has committed its multikey write
                    cv.wait(lock, [&numMultikeyCalls]() { return numMultikeyCalls == 2; });
                });

            // Set the index to multikey with 'first' as paths.
            collection->setIndexIsMultikey(
                opCtx.get(), indexEntry->descriptor()->indexName(), first);
            wuow.commit();
        });

        // Wait for the thread above to commit its multikey write to the storage engine
        {
            stdx::unique_lock lock(mutex);
            cv.wait(lock, [&numMultikeyCalls]() { return numMultikeyCalls == 1; });
        }

        // Set the index to multikey with 'second' as paths. This will not cause a WCE as the write
        // in the thread is fully committed to the storage engine.
        {
            Lock::GlobalLock globalLock{operationContext(), MODE_IX};
            // First confirm that we can observe the multikey write set by the other thread.
            MultikeyPaths paths;
            ASSERT_TRUE(collection->isIndexMultikey(
                operationContext(), indexEntry->descriptor()->indexName(), &paths));
            assertMultikeyPathsAreEqual(paths, first);

            // Then perform our own multikey write.
            WriteUnitOfWork wuow(operationContext());
            collection->setIndexIsMultikey(
                operationContext(), indexEntry->descriptor()->indexName(), second);
            wuow.commit();
        }

        // Notify the thread that our multikey write is committed
        {
            stdx::unique_lock lock(mutex);
            numMultikeyCalls = 2;
            cv.notify_all();
        }
        t.join();

        // Verify that our Collection instance has 'expected' as multikey paths for this index
        {
            MultikeyPaths multikeyPaths;
            ASSERT(collection->isIndexMultikey(
                operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
            assertMultikeyPathsAreEqual(multikeyPaths, expected);
        }

        // Verify that the durable catalog has 'expected' as multikey paths for this index
        Lock::GlobalLock globalLock{operationContext(), MODE_IS};
        auto md = durable_catalog::getParsedCatalogEntry(
                      operationContext(), collection->getCatalogId(), getMDBCatalog())
                      ->metadata;

        auto indexOffset = md->findIndexOffset(indexEntry->descriptor()->indexName());
        assertMultikeyPathsAreEqual(md->indexes[indexOffset].multikeyPaths, expected);
    }
};

TEST_F(ConcurrentMultikeyTest, MultikeyPathsConcurrentSecondSubset) {
    testConcurrentMultikey(BSON("a.b" << 1), {{0U, 1U}}, {{0U}}, {{0U, 1U}});
}

TEST_F(ConcurrentMultikeyTest, MultikeyPathsConcurrentDistinct) {
    testConcurrentMultikey(BSON("a.b" << 1), {{0U}}, {{1U}}, {{0U, 1U}});
}

TEST_F(DurableCatalogTest, SinglePhaseIndexBuild) {
    auto indexEntry = createIndex(BSON("a" << 1));
    auto collection = getCollection();

    ASSERT_FALSE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_FALSE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));

    {
        Lock::DBLock dbLk(operationContext(), collection->ns().dbName(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), collection->ns(), MODE_X);

        WriteUnitOfWork wuow(operationContext());
        getCollectionWriter()
            .getWritableCollection(operationContext())
            ->indexBuildSuccess(operationContext(), indexEntry);
        wuow.commit();
    }

    collection = getCollection();
    ASSERT_TRUE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_FALSE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));
}

TEST_F(DurableCatalogTest, TwoPhaseIndexBuild) {
    bool twoPhase = true;
    auto indexEntry = createIndex(BSON("a" << 1), IndexNames::BTREE, twoPhase);
    auto collection = getCollection();

    ASSERT_FALSE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_TRUE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));

    {
        Lock::DBLock dbLk(operationContext(), collection->ns().dbName(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), collection->ns(), MODE_X);

        WriteUnitOfWork wuow(operationContext());
        getCollectionWriter()
            .getWritableCollection(operationContext())
            ->indexBuildSuccess(operationContext(), indexEntry);
        wuow.commit();
    }

    collection = getCollection();
    ASSERT_TRUE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_FALSE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   CannotSetIndividualPathComponentsOfTextIndexAsMultikey,
                   R"#(Invariant failure.*multikeyPaths.empty\(\))#") {
    std::string indexType = IndexNames::TEXT;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();

    Lock::GlobalLock globalLock{operationContext(), MODE_IX};
    WriteUnitOfWork wuow(operationContext());
    collection->setIndexIsMultikey(
        operationContext(), indexEntry->descriptor()->indexName(), {{0U}, {0U}});
}

TEST_F(ImportCollectionTest, ImportCollection) {
    // Import should fail with empty metadata.
    ASSERT_THROWS_CODE(
        importCollectionTest(nss, {}, storageMetadata), AssertionException, ErrorCodes::BadValue);

    md->options.uuid = UUID::gen();
    auto mdObj = md->toBSON();
    auto idxIdentObj = BSON(IndexConstants::kIdIndexName << idxIdent);

    // Import should fail with missing "md" field.
    ASSERT_THROWS_CODE(
        importCollectionTest(
            nss,
            BSON("idxIdent" << idxIdentObj << "ns" << nss.ns_forTest() << "ident" << ident),
            storageMetadata),
        AssertionException,
        ErrorCodes::BadValue);

    // Import should fail with missing "ident" field.
    ASSERT_THROWS_CODE(importCollectionTest(nss,
                                            BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns"
                                                      << nss.ns_forTest()),
                                            storageMetadata),
                       AssertionException,
                       ErrorCodes::BadValue);

    // Import should success with validate inputs.
    auto swImportResult =
        importCollectionTest(nss,
                             BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns"
                                       << nss.ns_forTest() << "ident" << ident),
                             storageMetadata);
    ASSERT_OK(swImportResult.getStatus());
    durable_catalog::ImportResult importResult = std::move(swImportResult.getValue());

    Lock::GlobalLock globalLock{operationContext(), MODE_IS};

    // Validate the catalog entry for the imported collection.
    auto entry = getMDBCatalog()->getEntry(importResult.catalogId);
    ASSERT_EQ(entry.nss, nss);
    ASSERT_EQ(entry.ident, ident);
    ASSERT_EQ(getMDBCatalog()->getIndexIdent(
                  operationContext(), importResult.catalogId, IndexConstants::kIdIndexName),
              idxIdent);

    // Test that a collection UUID is generated for import.
    ASSERT_NE(md->options.uuid.value(), importResult.uuid);
    // Substitute in the generated UUID and check that the rest of fields in the catalog entry
    // match.
    md->options.uuid = importResult.uuid;
    ASSERT_BSONOBJ_EQ(
        getMDBCatalog()->getRawCatalogEntry(operationContext(), importResult.catalogId),
        BSON("md" << md->toBSON() << "idxIdent" << idxIdentObj << "ns" << nss.ns_forTest()
                  << "ident" << ident));
}

TEST_F(ImportCollectionTest, ImportCollectionNamespaceExists) {
    createCollection(nss, {});

    // Import should fail if the namespace already exists.
    ASSERT_THROWS_CODE(importCollectionTest(ns(), {}, storageMetadata),
                       AssertionException,
                       ErrorCodes::NamespaceExists);
}

TEST_F(DurableCatalogTest, CheckTimeseriesBucketsMayHaveMixedSchemaDataFlagFCVLatest) {
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
    auto mdbCatalog = getMDBCatalog();
    {
        const NamespaceString regularNss =
            NamespaceString::createNamespaceString_forTest("test.regular");
        createCollection(regularNss, CollectionOptions());

        Lock::GlobalLock globalLock{operationContext(), MODE_IS};
        auto collection = CollectionCatalog::get(operationContext())
                              ->lookupCollectionByNamespace(operationContext(), regularNss);
        RecordId catalogId = collection->getCatalogId();
        ASSERT(!durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, mdbCatalog)
                    ->metadata->timeseriesBucketsMayHaveMixedSchemaData);
    }

    {
        const NamespaceString bucketsNss =
            NamespaceString::createNamespaceString_forTest("system.buckets.ts");
        CollectionOptions options;
        options.timeseries = TimeseriesOptions(/*timeField=*/"t");
        createCollection(bucketsNss, options);

        Lock::GlobalLock globalLock{operationContext(), MODE_IS};
        auto collection = CollectionCatalog::get(operationContext())
                              ->lookupCollectionByNamespace(operationContext(), bucketsNss);
        RecordId catalogId = collection->getCatalogId();
        ASSERT(durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, mdbCatalog)
                   ->metadata->timeseriesBucketsMayHaveMixedSchemaData);
        ASSERT_FALSE(
            *durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, mdbCatalog)
                 ->metadata->timeseriesBucketsMayHaveMixedSchemaData);
    }
}

TEST_F(DurableCatalogTest, CreateCollectionCatalogEntryHasCorrectTenantNamespace) {
    gMultitenancySupport = true;

    auto tenantId = TenantId(OID::gen());
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, "test.regular");
    createCollection(nss, CollectionOptions());

    auto collection = CollectionCatalog::get(operationContext())
                          ->lookupCollectionByNamespace(operationContext(), nss);
    RecordId catalogId = collection->getCatalogId();
    auto mdbCatalog = getMDBCatalog();
    ASSERT_EQ(mdbCatalog->getEntry(catalogId).nss.tenantId(), nss.tenantId());
    ASSERT_EQ(mdbCatalog->getEntry(catalogId).nss, nss);

    Lock::GlobalLock globalLock{operationContext(), MODE_IS};
    ASSERT_EQ(durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, mdbCatalog)
                  ->metadata->nss.tenantId(),
              nss.tenantId());
    ASSERT_EQ(durable_catalog::getParsedCatalogEntry(operationContext(), catalogId, mdbCatalog)
                  ->metadata->nss,
              nss);

    auto catalogEntry =
        durable_catalog::scanForCatalogEntryByNss(operationContext(), nss, mdbCatalog);

    gMultitenancySupport = false;
}


TEST_F(DurableCatalogTest, ScanForCatalogEntryByNssBasic) {
    gMultitenancySupport = true;
    ON_BLOCK_EXIT([&] { gMultitenancySupport = false; });

    /**
     * Create some collections for which to scan.
     */

    auto tenantId = TenantId(OID::gen());
    const NamespaceString nssFirst =
        NamespaceString::createNamespaceString_forTest(tenantId, "test.first");
    auto catalogIdAndUUIDFirst = createCollection(nssFirst, CollectionOptions());

    const NamespaceString nssSecond =
        NamespaceString::createNamespaceString_forTest("system.buckets.ts");
    CollectionOptions options;
    options.timeseries = TimeseriesOptions(/*timeField=*/"t");
    auto catalogIdAndUUIDSecond = createCollection(nssSecond, options);

    const NamespaceString nssThird = NamespaceString::createNamespaceString_forTest("test.third");
    auto catalogIdAndUUIDThird = createCollection(nssThird, CollectionOptions());

    /**
     * Fetch catalog entries by namespace by scanning the mdb catalog.
     */

    auto mdbCatalog = getMDBCatalog();

    // Need a read lock for durable_catalog calls.
    Lock::GlobalLock globalLock{operationContext(), MODE_IS};
    auto catalogEntryThird =
        durable_catalog::scanForCatalogEntryByNss(operationContext(), nssThird, mdbCatalog);
    ASSERT(catalogEntryThird != boost::none);
    ASSERT_EQ(nssThird, catalogEntryThird->metadata->nss);
    ASSERT_EQ(catalogIdAndUUIDThird.uuid, catalogEntryThird->metadata->options.uuid);
    ASSERT_EQ(durable_catalog::getParsedCatalogEntry(
                  operationContext(), catalogIdAndUUIDThird.catalogId, mdbCatalog)
                  ->metadata->nss,
              nssThird);
    ASSERT_EQ(mdbCatalog->getEntry(catalogIdAndUUIDThird.catalogId).nss, nssThird);

    auto catalogEntrySecond =
        durable_catalog::scanForCatalogEntryByNss(operationContext(), nssSecond, mdbCatalog);
    ASSERT(catalogEntrySecond != boost::none);
    ASSERT_EQ(nssSecond, catalogEntrySecond->metadata->nss);
    ASSERT_EQ(catalogIdAndUUIDSecond.uuid, catalogEntrySecond->metadata->options.uuid);
    ASSERT(catalogEntrySecond->metadata->options.timeseries);
    ASSERT_EQ(durable_catalog::getParsedCatalogEntry(
                  operationContext(), catalogIdAndUUIDSecond.catalogId, mdbCatalog)
                  ->metadata->nss,
              nssSecond);
    ASSERT_EQ(mdbCatalog->getEntry(catalogIdAndUUIDSecond.catalogId).nss, nssSecond);

    auto catalogEntryFirst =
        durable_catalog::scanForCatalogEntryByNss(operationContext(), nssFirst, mdbCatalog);
    ASSERT(catalogEntryFirst != boost::none);
    ASSERT_EQ(nssFirst, catalogEntryFirst->metadata->nss);
    ASSERT_EQ(catalogIdAndUUIDFirst.uuid, catalogEntryFirst->metadata->options.uuid);
    ASSERT_EQ(nssFirst.tenantId(), catalogEntryFirst->metadata->nss.tenantId());
    ASSERT_EQ(durable_catalog::getParsedCatalogEntry(
                  operationContext(), catalogIdAndUUIDFirst.catalogId, mdbCatalog)
                  ->metadata->nss,
              nssFirst);
    ASSERT_EQ(mdbCatalog->getEntry(catalogIdAndUUIDFirst.catalogId).nss, nssFirst);

    auto catalogEntryDoesNotExist = durable_catalog::scanForCatalogEntryByNss(
        operationContext(),
        NamespaceString::createNamespaceString_forTest("foo", "bar"),
        mdbCatalog);
    ASSERT(catalogEntryDoesNotExist == boost::none);
}

TEST_F(DurableCatalogTest, CreateCollectionSucceedsWithExistingIdent) {
    auto opCtx = operationContext();
    auto mdbCatalog = getMDBCatalog();
    const auto catalogId = mdbCatalog->reserveCatalogId(operationContext());

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto ident = storageEngine->generateNewIndexIdent(nss.dbName());
    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        auto recordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
        wuow.commit();
    }
    auto parsedEntry = mdbCatalog->getEntry(catalogId);
    ASSERT_EQUALS(catalogId, parsedEntry.catalogId);
    ASSERT_EQUALS(nss, parsedEntry.nss);
    ASSERT_EQUALS(ident, parsedEntry.ident);

    // Remove the catalog entry to simulate the first phase of a two phase collection drop. This
    // mimics a scenario where a 'create' operation with replicated catalog identifiers is applied,
    // rolled back, and reapplied.
    {

        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(mdbCatalog->removeEntry(opCtx, catalogId));
        storageEngine->addDropPendingIdent(Timestamp(), std::make_shared<Ident>(ident));
        wuow.commit();
    }

    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        auto recordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
        wuow.commit();
    }
    parsedEntry = mdbCatalog->getEntry(catalogId);
    ASSERT_EQUALS(catalogId, parsedEntry.catalogId);
    ASSERT_EQUALS(nss, parsedEntry.nss);
    ASSERT_EQUALS(ident, parsedEntry.ident);
}

TEST_F(DurableCatalogTest, CreateCollectionRemovesPriorDocumentsAfterRecreate) {
    auto opCtx = operationContext();
    auto mdbCatalog = getMDBCatalog();
    const auto catalogId = mdbCatalog->reserveCatalogId(operationContext());

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto ident = storageEngine->generateNewIndexIdent(nss.dbName());

    std::unique_ptr<RecordStore> collectionRecordStore;
    // 1. Create the collection entry in the catalog.
    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        collectionRecordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
        wuow.commit();
    }
    auto parsedEntry = mdbCatalog->getEntry(catalogId);
    ASSERT_EQUALS(catalogId, parsedEntry.catalogId);
    ASSERT_EQUALS(nss, parsedEntry.nss);
    ASSERT_EQUALS(ident, parsedEntry.ident);


    // 2. Insert a document into the newly created collection.
    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        RecordId rid(1);
        BSONObj obj = BSON("_id" << 1 << "x"
                                 << "doc1");
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        ASSERT_OK(collectionRecordStore
                      ->insertRecord(opCtx, ru, obj.objdata(), obj.objsize(), Timestamp())
                      .getStatus());
        wuow.commit();
    }

    // Confirm document is present before catalog removal.
    {
        auto collection = acquireCollectionForRead(opCtx, nss);
        auto cursor =
            collectionRecordStore->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        ASSERT(cursor->next());
    }

    // 3. Remove the catalog entry simulating phase one of a two-phase drop.
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(mdbCatalog->removeEntry(opCtx, catalogId));
        storageEngine->addDropPendingIdent(Timestamp(), std::make_shared<Ident>(ident));
        wuow.commit();
    }

    // 4. Recreate the catalog entry for the same ident with a new UUID.
    std::unique_ptr<RecordStore> newCollectionRecordStore;
    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        newCollectionRecordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
        wuow.commit();
    }
    parsedEntry = mdbCatalog->getEntry(catalogId);
    ASSERT_EQUALS(catalogId, parsedEntry.catalogId);
    ASSERT_EQUALS(nss, parsedEntry.nss);
    ASSERT_EQUALS(ident, parsedEntry.ident);

    // 5. Confirm documents inserted before catalog drop are now gone.
    {
        auto collection = acquireCollectionForRead(opCtx, nss);
        auto cursor =
            newCollectionRecordStore->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        ASSERT(!cursor->next());
    }
}

TEST_F(DurableCatalogTest, CreateCollectionWithFailsExistingIdentButDifferentCatalogId) {
    auto opCtx = operationContext();
    auto mdbCatalog = getMDBCatalog();
    const auto catalogId = mdbCatalog->reserveCatalogId(operationContext());

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto ident = storageEngine->generateNewIndexIdent(nss.dbName());

    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        auto recordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
        wuow.commit();
    }
    const auto parsedEntry = mdbCatalog->getEntry(catalogId);
    ASSERT_EQUALS(catalogId, parsedEntry.catalogId);
    ASSERT_EQUALS(nss, parsedEntry.nss);
    ASSERT_EQUALS(ident, parsedEntry.ident);

    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        auto newCatalogId = mdbCatalog->reserveCatalogId(operationContext());
        WriteUnitOfWork wuow(opCtx);
        ASSERT_EQUALS(
            ErrorCodes::ObjectAlreadyExists,
            durable_catalog::createCollection(
                opCtx, newCatalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog)
                .getStatus());
    }
}

TEST_F(DurableCatalogTest, CreateCollectionWithCatalogIdentifierSucceedsAfterRollback) {
    auto opCtx = operationContext();
    auto mdbCatalog = getMDBCatalog();
    const auto catalogId = mdbCatalog->reserveCatalogId(operationContext());

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto ident = storageEngine->generateNewIndexIdent(nss.dbName());

    {
        // Abort the first attempt to create the collection.
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        auto recordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
    }
    ASSERT_FALSE(mdbCatalog->getEntry_forTest(catalogId));

    {
        auto collection = acquireCollectionForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        auto recordStore = unittest::assertGet(durable_catalog::createCollection(
            opCtx, catalogId, nss, ident, CollectionOptions{.uuid = UUID::gen()}, mdbCatalog));
        wuow.commit();
    }
    const auto parsedEntry = mdbCatalog->getEntry(catalogId);
    ASSERT_EQUALS(catalogId, parsedEntry.catalogId);
    ASSERT_EQUALS(nss, parsedEntry.nss);
    ASSERT_EQUALS(ident, parsedEntry.ident);
}

}  // namespace
}  // namespace mongo
