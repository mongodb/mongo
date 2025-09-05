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


#include "mongo/db/index_builds/index_builds_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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

CollectionAcquisition getCollectionExclusive(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
}

void IndexBuildsCoordinatorTest::createCollectionWithDuplicateDocs(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    // Create collection.
    CollectionOptions options;
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, options));

    auto collection = getCollectionExclusive(opCtx, nss);
    invariant(collection.exists());

    // Insert some data.
    OpDebug* const nullOpDebug = nullptr;
    for (int i = 0; i < 10; i++) {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocument(opCtx,
                                                      collection.getCollectionPtr(),
                                                      InsertStatement(BSON("_id" << i << "a" << 1)),
                                                      nullOpDebug));
        wuow.commit();
    }

    ASSERT_EQ(collection.getCollectionPtr()->getIndexCatalog()->numIndexesTotal(), 1);
}

// Helper to refetch the Collection from the catalog in order to see any changes made to it
CollectionPtr coll(OperationContext* opCtx, const NamespaceString& nss) {
    // TODO(SERVER-103400): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
    return CollectionPtr::CollectionPtr_UNSAFE(
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundUniqueEnforce) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundUniqueEnforce");
    createCollectionWithDuplicateDocs(opCtx, nss);

    auto collection = getCollectionExclusive(opCtx, nss);
    ASSERT(collection.exists());
    auto indexKey = BSON("a" << 1);
    auto spec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    ASSERT_THROWS_CODE(indexBuildsCoord->createIndex(
                           opCtx, collection.uuid(), spec, indexConstraints, fromMigrate),
                       AssertionException,
                       ErrorCodes::DuplicateKey);
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 1);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundUniqueRelax) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundUniqueRelax");
    createCollectionWithDuplicateDocs(opCtx, nss);

    auto collection = getCollectionExclusive(opCtx, nss);
    ASSERT(collection.exists());
    auto indexKey = BSON("a" << 1);
    auto spec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    auto fromMigrate = false;
    ASSERT_DOES_NOT_THROW(indexBuildsCoord->createIndex(
        opCtx, collection.uuid(), spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexAlreadyExists) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundIndexAlreadyExists");
    createCollectionWithDuplicateDocs(opCtx, nss);

    auto collection = getCollectionExclusive(opCtx, nss);
    ASSERT(collection.exists());
    auto indexKey = BSON("a" << 1);
    auto spec = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                         << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    auto uuid = collection.uuid();
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);

    // Should silently return if the index already exists.
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexOptionsConflictEnforce) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundIndexOptionsConflictEnforce");
    createCollectionWithDuplicateDocs(opCtx, nss);

    auto collection = getCollectionExclusive(opCtx, nss);
    ASSERT(collection.exists());
    auto indexKey = BSON("a" << 1);
    auto spec1 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto spec2 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_2"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    auto uuid = collection.uuid();
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec1, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);

    ASSERT_THROWS_CODE(
        indexBuildsCoord->createIndex(opCtx, uuid, spec2, indexConstraints, fromMigrate),
        AssertionException,
        ErrorCodes::IndexOptionsConflict);
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexOptionsConflictRelax) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundIndexOptionsConflictRelax");
    createCollectionWithDuplicateDocs(opCtx, nss);

    auto collection = getCollectionExclusive(opCtx, nss);
    ASSERT(collection.exists());
    auto indexKey = BSON("a" << 1);
    auto spec1 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto spec2 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_2"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    auto fromMigrate = false;
    auto uuid = collection.uuid();
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec1, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);

    // Should silently return in relax mode even if there are index option conflicts.
    ASSERT_DOES_NOT_THROW(
        indexBuildsCoord->createIndex(opCtx, uuid, spec2, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);
}

class OpObserverMock : public OpObserverNoop {
public:
    std::vector<std::string> createIndexIdents;
    std::vector<std::string> startIndexBuildIdents;

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexBuildInfo& indexBuildInfo,
                       bool fromMigrate,
                       bool isViewlessTimeseries) override {
        createIndexIdents.emplace_back(indexBuildInfo.indexIdent);
    }

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isViewlessTimeseries) override {
        for (const auto& indexBuildInfo : indexes) {
            startIndexBuildIdents.push_back(indexBuildInfo.indexIdent);
        }
    }

    static OpObserverMock* install(OperationContext* opCtx) {
        auto opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(opCtx->getServiceContext()->getOpObserver());
        auto mockObserver = std::make_unique<OpObserverMock>();
        auto opObserver = mockObserver.get();
        opObserverRegistry->addObserver(std::move(mockObserver));
        return opObserver;
    }
};

TEST_F(IndexBuildsCoordinatorTest, CreateIndexOnEmptyCollectionReplicatesIdent) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), &collection};
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(),
            writer,
            {BSON("v" << 2 << "key" << BSON("a" << 1) << "name"
                      << "a_1")},
            false);
        wuow.commit();
    }

    // Verify that the op observer was called and that it was given the correct ident
    auto collection = getCollectionExclusive(operationContext(), nss);
    ASSERT_EQ(opObserver->createIndexIdents.size(), 1);
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        operationContext(), opObserver->createIndexIdents[0]));
}

TEST_F(IndexBuildsCoordinatorTest, CreateIndexOnNonEmptyCollectionReplicatesIdent) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(operationContext(),
                                                      collection.getCollectionPtr(),
                                                      InsertStatement(BSON("_id" << 1 << "a" << 1)),
                                                      nullptr));
        wuow.commit();

        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        indexBuildsCoord->createIndex(operationContext(),
                                      collection.uuid(),
                                      BSON("v" << 2 << "key" << BSON("a" << 1) << "name"
                                               << "a_1"),
                                      IndexBuildsManager::IndexConstraints::kRelax,
                                      false);
    }

    // Verify that the op observer was called and that it was given the correct ident
    auto collection = getCollectionExclusive(operationContext(), nss);
    ASSERT_EQ(opObserver->createIndexIdents.size(), 1);
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        operationContext(), opObserver->createIndexIdents[0]));
}

TEST_F(IndexBuildsCoordinatorTest, CreateIndexUsesSpecifiedIdent) {
    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnNonEmptyCollectionUsesSpecifiedIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(operationContext(),
                                                      collection.getCollectionPtr(),
                                                      InsertStatement(BSON("_id" << 1 << "a" << 1)),
                                                      nullptr));
        wuow.commit();

        auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                           std::string{"index-ident"});
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexBuildsCoord->createIndex(operationContext(),
                                      collection.uuid(),
                                      indexBuildInfo,
                                      IndexBuildsManager::IndexConstraints::kRelax);
    }

    auto collection = getCollectionExclusive(operationContext(), nss);
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                                              "index-ident"));
}

TEST_F(IndexBuildsCoordinatorTest, CreateIndexWithExistingIdentReportsError) {
    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexWithExistingIdentReportsError");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());

    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                           std::string{"index-ident"});
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexBuildsCoord->createIndex(operationContext(),
                                      collection.uuid(),
                                      indexBuildInfo,
                                      IndexBuildsManager::IndexConstraints::kRelax);
    }

    {
        // This succeeds because it's an exact match for the existing index and so is a no-op
        auto collection = getCollectionExclusive(operationContext(), nss);
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                           std::string{"index-ident"});
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexBuildsCoord->createIndex(operationContext(),
                                      collection.uuid(),
                                      indexBuildInfo,
                                      IndexBuildsManager::IndexConstraints::kRelax);
    }

    {
        // This fails because it's a different index with the same ident
        auto collection = getCollectionExclusive(operationContext(), nss);
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"),
                           std::string{"index-ident"});
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        ASSERT_THROWS_CODE(
            indexBuildsCoord->createIndex(operationContext(),
                                          collection.uuid(),
                                          indexBuildInfo,
                                          IndexBuildsManager::IndexConstraints::kRelax),
            DBException,
            ErrorCodes::ObjectAlreadyExists);
    }
}

TEST_F(IndexBuildsCoordinatorTest, RetryIndexCreation) {
    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.RetryCreationOfIndex");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        CollectionWriter writer(operationContext(), &collection);
        WriteUnitOfWork wuow(operationContext());

        std::vector<IndexBuildInfo> indexes;
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                           std::string{"index-ident"});
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexes.push_back(std::move(indexBuildInfo));
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(), writer, std::span<IndexBuildInfo>{indexes}, false);
    }

    // Write wasn't committed so the ident creation should have been rolled back and creating a
    // different index with the same ident should work

    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        CollectionWriter writer(operationContext(), &collection);
        WriteUnitOfWork wuow(operationContext());
        std::vector<IndexBuildInfo> indexes;
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"),
                           std::string{"index-ident"});
        indexBuildInfo.setInternalIdents(*storageEngine,
                                         VersionContext::getDecoration(operationContext()));
        indexes.push_back(std::move(indexBuildInfo));
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(), writer, std::span<IndexBuildInfo>{indexes}, false);
        wuow.commit();
    }
}

TEST_F(IndexBuildsCoordinatorTest, StartIndexBuildOnEmptyCollectionReplicatesAsCreateIndex) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
    auto collectionUUID = getCollectionExclusive(operationContext(), nss).uuid();

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    std::vector<IndexBuildInfo> indexes;
    auto indexBuildInfo = IndexBuildInfo(
        BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"), std::string{"index-1"});
    indexBuildInfo.setInternalIdents(*storageEngine,
                                     VersionContext::getDecoration(operationContext()));
    indexes.push_back(std::move(indexBuildInfo));
    indexBuildInfo = IndexBuildInfo(BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"),
                                    std::string{"index-2"});
    indexBuildInfo.setInternalIdents(*storageEngine,
                                     VersionContext::getDecoration(operationContext()));
    indexes.push_back(std::move(indexBuildInfo));

    {
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto status = indexBuildsCoord->startIndexBuild(operationContext(),
                                                        nss.dbName(),
                                                        collectionUUID,
                                                        indexes,
                                                        UUID::gen(),
                                                        IndexBuildProtocol::kTwoPhase,
                                                        {});
        ASSERT_OK(status);
        status.getValue().wait();
    }

    // Verify that the op observer was called, that it was given the expected idents, and that the
    // specified idents were actually used. Since the collection was empty, onCreateIndex() was
    // called instead of onStartIndexBuild.
    std::vector<std::string> idents;
    for (auto& indexBuildInfo : indexes) {
        idents.push_back(indexBuildInfo.indexIdent);
    }

    auto collection = getCollectionExclusive(operationContext(), nss);
    ASSERT_EQ(opObserver->createIndexIdents, idents);
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        operationContext(), opObserver->createIndexIdents[0]));
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        operationContext(), opObserver->createIndexIdents[1]));
}

TEST_F(IndexBuildsCoordinatorTest, StartIndexBuildOnNonEmptyCollectionReplicatesIdents) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), NamespaceString::kIndexBuildEntryNamespace, CollectionOptions()));

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    std::vector<IndexBuildInfo> indexes;
    auto indexBuildInfo = IndexBuildInfo(
        BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"), std::string{"index-1"});
    indexBuildInfo.setInternalIdents(*storageEngine,
                                     VersionContext::getDecoration(operationContext()));
    indexes.push_back(std::move(indexBuildInfo));
    indexBuildInfo = IndexBuildInfo(BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"),
                                    std::string{"index-2"});
    indexBuildInfo.setInternalIdents(*storageEngine,
                                     VersionContext::getDecoration(operationContext()));
    indexes.push_back(std::move(indexBuildInfo));

    auto collUUID = [&] {
        auto collection = getCollectionExclusive(operationContext(), nss);

        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(operationContext(),
                                                      collection.getCollectionPtr(),
                                                      InsertStatement(BSON("_id" << 1 << "a" << 1)),
                                                      nullptr));
        wuow.commit();

        return collection.uuid();
    }();

    {
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto buildUUID = UUID::gen();
        auto buildFuture = unittest::assertGet(indexBuildsCoord->startIndexBuild(
            operationContext(),
            nss.dbName(),
            collUUID,
            indexes,
            buildUUID,
            IndexBuildProtocol::kTwoPhase,
            IndexBuildsCoordinator::IndexBuildOptions{.commitQuorum = CommitQuorumOptions(1)}));
        ASSERT_OK(indexBuildsCoord->voteCommitIndexBuild(
            operationContext(), buildUUID, HostAndPort("test1", 1234)));
        ASSERT_OK(buildFuture.getNoThrow());
    }

    // Verify that the op observer was called, that it was given the expected idents, and that the
    // specified idents were actually used.
    std::vector<std::string> idents;
    for (auto& indexBuildInfo : indexes) {
        idents.push_back(indexBuildInfo.indexIdent);
    }

    auto collection = getCollectionExclusive(operationContext(), nss);
    ASSERT_EQ(opObserver->startIndexBuildIdents, idents);
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        operationContext(), opObserver->startIndexBuildIdents[0]));
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        operationContext(), opObserver->startIndexBuildIdents[1]));
}

}  // namespace
}  // namespace mongo
