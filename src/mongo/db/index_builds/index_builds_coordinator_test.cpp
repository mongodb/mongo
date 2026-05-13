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
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
    for (int i = 0; i < 10; i++) {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            Helpers::insert(opCtx, collection.getCollectionPtr(), BSON("_id" << i << "a" << 1)));
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
    auto spec = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                         << (std::string(indexKey.firstElementFieldNameStringData()) + "_1")
                         << "unique" << true);
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
    auto spec = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                         << (std::string(indexKey.firstElementFieldNameStringData()) + "_1")
                         << "unique" << true);
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
                         << (std::string(indexKey.firstElementFieldNameStringData()) + "_1"));
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
                          << (std::string(indexKey.firstElementFieldNameStringData()) + "_1"));
    auto spec2 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (std::string(indexKey.firstElementFieldNameStringData()) + "_2"));
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
                          << (std::string(indexKey.firstElementFieldNameStringData()) + "_1"));
    auto spec2 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (std::string(indexKey.firstElementFieldNameStringData()) + "_2"));
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

TEST_F(IndexBuildsCoordinatorTest, GetNumIndexesTotalReturnsCatalogCount) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.GetNumIndexesTotalReturnsCatalogCount");
    createCollectionWithDuplicateDocs(opCtx, nss);

    {
        auto collection = getCollectionExclusive(opCtx, nss);
        EXPECT_EQ(1,
                  IndexBuildsCoordinator::getNumIndexesTotal(opCtx, collection.getCollectionPtr()));
    }

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto spec = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("a" << 1)
                         << "name" << "a_1");
    {
        auto collection = getCollectionExclusive(opCtx, nss);
        ASSERT_DOES_NOT_THROW(indexBuildsCoord->createIndex(
            opCtx, collection.uuid(), spec, IndexBuildsManager::IndexConstraints::kRelax, false));
    }

    {
        auto collection = getCollectionExclusive(opCtx, nss);
        EXPECT_EQ(2,
                  IndexBuildsCoordinator::getNumIndexesTotal(opCtx, collection.getCollectionPtr()));
    }
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
        ASSERT_OK(Helpers::insert(
            operationContext(), collection.getCollectionPtr(), BSON("_id" << 1 << "a" << 1)));
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
        ASSERT_OK(Helpers::insert(
            operationContext(), collection.getCollectionPtr(), BSON("_id" << 1 << "a" << 1)));
        wuow.commit();

        auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                           "index-ident",
                           *storageEngine);
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
                           "index-ident",
                           *storageEngine);
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
                           "index-ident",
                           *storageEngine);
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
                           "index-ident",
                           *storageEngine);
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
        indexes.emplace_back(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                             "index-ident",
                             *storageEngine);
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(), writer, indexes, false);
    }

    // Write wasn't committed so the ident creation should have been rolled back and creating a
    // different index with the same ident should work

    {
        auto collection = getCollectionExclusive(operationContext(), nss);
        CollectionWriter writer(operationContext(), &collection);
        WriteUnitOfWork wuow(operationContext());
        std::vector<IndexBuildInfo> indexes;
        indexes.emplace_back(BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"),
                             "index-ident",
                             *storageEngine);
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(), writer, indexes, false);
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
    indexes.emplace_back(
        BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"), "index-1", *storageEngine);
    indexes.emplace_back(
        BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"), "index-2", *storageEngine);

    {
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto status = indexBuildsCoord->startIndexBuild(
            operationContext(), nss.dbName(), collectionUUID, indexes, UUID::gen(), {});
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
    indexes.emplace_back(
        BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"), "index-1", *storageEngine);
    indexes.emplace_back(
        BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"), "index-2", *storageEngine);

    auto collUUID = [&] {
        auto collection = getCollectionExclusive(operationContext(), nss);

        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(
            operationContext(), collection.getCollectionPtr(), BSON("_id" << 1 << "a" << 1)));
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

// Creates a single phase and  primary-driven index build and checks that
// 'abortAllTwoPhaseIndexBuildsForStepUp' aborts the index build.
TEST_F(IndexBuildsCoordinatorTest, StepUpPrimaryDrivenAbortsOnlyTwoPhaseBuilds) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    auto* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

    const auto twoPhaseNss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.StepUpPrimaryDrivenAbortsOnlyTwoPhaseBuilds.twoPhase");
    ASSERT_OK(storageInterface()->createCollection(opCtx, twoPhaseNss, CollectionOptions()));
    auto status = storageInterface()->createCollection(
        opCtx, NamespaceString::kIndexBuildEntryNamespace, CollectionOptions());
    if (status != ErrorCodes::NamespaceExists) {
        ASSERT_OK(status);
    }

    const auto singlePhaseNss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.StepUpPrimaryDrivenAbortsOnlyTwoPhaseBuilds.singlePhase");
    ASSERT_OK(storageInterface()->createCollection(opCtx, singlePhaseNss, CollectionOptions()));

    // Avoid the empty collection index build optimization.
    auto twoPhaseUUID = [&] {
        auto collection = getCollectionExclusive(opCtx, twoPhaseNss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, collection.getCollectionPtr(), BSON("_id" << 1)));
        wuow.commit();
        return collection.uuid();
    }();

    auto singlePhaseUUID = [&] {
        auto collection = getCollectionExclusive(opCtx, singlePhaseNss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, collection.getCollectionPtr(), BSON("_id" << 1)));
        wuow.commit();
        return collection.uuid();
    }();

    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto twoPhaseIndexes = toIndexBuildInfoVec(
        std::vector<BSONObj>{BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1")},
        *storageEngine,
        twoPhaseNss.dbName());
    auto singlePhaseIndexes = toIndexBuildInfoVec(
        std::vector<BSONObj>{BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1")},
        *storageEngine,
        singlePhaseNss.dbName());

    // Create a window to enumerate the index builds while they are running.
    indexBuildsCoord->sleepIndexBuilds_forTestOnly(true);

    IndexBuildsCoordinator::IndexBuildOptions twoPhaseOptions;
    twoPhaseOptions.indexBuildProtocol = IndexBuildProtocol::kPrimaryDriven;
    twoPhaseOptions.commitQuorum = CommitQuorumOptions(1);
    auto twoPhaseFuture = unittest::assertGet(indexBuildsCoord->startIndexBuild(
        opCtx, twoPhaseNss.dbName(), twoPhaseUUID, twoPhaseIndexes, UUID::gen(), twoPhaseOptions));

    IndexBuildsCoordinator::IndexBuildOptions singlePhaseOptions;
    singlePhaseOptions.indexBuildProtocol = IndexBuildProtocol::kSinglePhase;
    singlePhaseOptions.commitQuorum = CommitQuorumOptions(1);
    auto singlePhaseFuture =
        unittest::assertGet(indexBuildsCoord->startIndexBuild(opCtx,
                                                              singlePhaseNss.dbName(),
                                                              singlePhaseUUID,
                                                              singlePhaseIndexes,
                                                              UUID::gen(),
                                                              singlePhaseOptions));

    ASSERT_TRUE(
        indexBuildsCoord->inProgForCollection(twoPhaseUUID, IndexBuildProtocol::kPrimaryDriven));
    ASSERT_TRUE(
        indexBuildsCoord->inProgForCollection(singlePhaseUUID, IndexBuildProtocol::kSinglePhase));

    indexBuildsCoord->sleepIndexBuilds_forTestOnly(false);

    indexBuildsCoord->abortAllTwoPhaseIndexBuildsForStepUp(
        opCtx,
        Status{ErrorCodes::InterruptedDueToReplStateChange, "aborting all two-phase index builds"});

    twoPhaseFuture.wait();

    ASSERT_THROWS_CODE(
        twoPhaseFuture.get(), DBException, ErrorCodes::InterruptedDueToReplStateChange);

    singlePhaseFuture.wait();
    auto catalogStats = singlePhaseFuture.get();
    ASSERT_GTE(catalogStats.numIndexesAfter, catalogStats.numIndexesBefore);

    // Both indexes should no longer exist after one completes and the other is aborted.
    indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(
        opCtx, twoPhaseUUID, IndexBuildProtocol::kTwoPhase);
    ASSERT_FALSE(
        indexBuildsCoord->inProgForCollection(twoPhaseUUID, IndexBuildProtocol::kTwoPhase));

    indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(
        opCtx, singlePhaseUUID, IndexBuildProtocol::kSinglePhase);
    ASSERT_FALSE(
        indexBuildsCoord->inProgForCollection(singlePhaseUUID, IndexBuildProtocol::kSinglePhase));
}

TEST_F(IndexBuildsCoordinatorTest, CommitRemovesBuildFromPrimaryDrivenRegistry) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CommitRemovesBuildFromPrimaryDrivenRegistry");
    ASSERT_OK(storageInterface()->createCollection(opCtx, ns, CollectionOptions{}));
    auto collUUID = [&] {
        auto collection = getCollectionExclusive(opCtx, ns);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, collection.getCollectionPtr(), BSON("_id" << 1)));
        wuow.commit();
        return collection.uuid();
    }();

    auto indexes = toIndexBuildInfoVec(
        std::vector<BSONObj>{BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1")},
        *opCtx->getServiceContext()->getStorageEngine(),
        ns.dbName());
    auto buildUUID = UUID::gen();

    auto& registry = index_builds::primary_driven::registry(opCtx->getServiceContext());
    registry.add(buildUUID, ns.dbName(), collUUID, indexes, boost::none);

    auto future = unittest::assertGet(IndexBuildsCoordinator::get(opCtx)->startIndexBuild(
        opCtx,
        ns.dbName(),
        collUUID,
        indexes,
        buildUUID,
        {.indexBuildMethod = IndexBuildMethodEnum::kPrimaryDriven,
         .indexBuildProtocol = IndexBuildProtocol::kPrimaryDriven,
         .commitQuorum = CommitQuorumOptions{CommitQuorumOptions::kPrimarySelfVote}}));
    ASSERT_OK(future.getNoThrow());

    EXPECT_TRUE(registry.all().empty());
}

// Persists a minimal kPrimaryDriven ResumeIndexInfo for `buildUUID` at the supplied phase,
// wired to the side-writes/skipped/sorter idents that `index_builds::primary_driven::start`
// already created via `indexes`. Lets resume-on-step-up tests stage the same setup without
// duplicating the IndexStateInfo wiring.
void persistPrimaryDrivenResumeState(OperationContext* opCtx,
                                     const std::vector<IndexBuildInfo>& indexes,
                                     const UUID& buildUUID,
                                     const UUID& collectionUUID,
                                     const std::string& resumeStateIdent,
                                     IndexBuildPhaseEnum phase) {
    ResumeIndexInfo resumeIndexInfo;
    resumeIndexInfo.setBuildUUID(buildUUID);
    resumeIndexInfo.setPhase(phase);
    resumeIndexInfo.setCollectionUUID(collectionUUID);

    std::vector<IndexStateInfo> indexStateInfos;
    for (auto&& indexBuildInfo : indexes) {
        IndexStateInfo indexInfo;
        indexInfo.setSpec(indexBuildInfo.spec);
        indexInfo.setIsMultikey(false);
        indexInfo.setMultikeyPaths({});
        indexInfo.setSideWritesTable(*indexBuildInfo.sideWritesIdent);
        indexInfo.setSkippedRecordTrackerTable(indexBuildInfo.skippedRecordsIdent);
        indexInfo.setStorageIdentifier(indexBuildInfo.sorterIdent);
        indexStateInfos.push_back(indexInfo);
    }
    resumeIndexInfo.setIndexes(indexStateInfos);
    auto obj = resumeIndexInfo.toBSON();

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    WriteUnitOfWork wuow(opCtx);
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto rs =
        storageEngine->getEngine()->getInternalRecordStore(ru, resumeStateIdent, KeyFormat::Long);
    ASSERT_OK(rs->insertRecord(opCtx, ru, obj.objdata(), obj.objsize(), Timestamp()));
    wuow.commit();
}

// Stages a two-index kPrimaryDriven build, persists resume state at `phase`, drives a
// step-up, and asserts the build resumed.
void runResumePrimaryDrivenOnStepUpTest(OperationContext* opCtx,
                                        repl::StorageInterface* storageInterface,
                                        StringData testName,
                                        IndexBuildPhaseEnum phase) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    // TODO(SERVER-124910): Remove.
    RAIIServerParameterControllerForTest ffPDIBResume(
        "featureFlagResumablePrimaryDrivenIndexBuilds", true);

    auto opObserver = OpObserverMock::install(opCtx);
    auto* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

    const auto nss = NamespaceString::createNamespaceString_forTest(
        std::string{"IndexBuildsCoordinatorTest."} + std::string{testName});
    ASSERT_OK(storageInterface->createCollection(opCtx, nss, CollectionOptions()));

    // Avoid the empty collection index build optimization.
    auto collectionUUID = [&] {
        auto collection = getCollectionExclusive(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, collection.getCollectionPtr(), BSON("_id" << 1)));
        wuow.commit();
        return collection.uuid();
    }();

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    std::vector<IndexBuildInfo> indexes;
    indexes.emplace_back(
        BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"), "index-1", *storageEngine);
    indexes.emplace_back(
        BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1"), "index-2", *storageEngine);

    auto buildUUID = UUID::gen();
    auto resumeStateIdent = ident::generateNewIndexBuildIdent(buildUUID);

    ASSERT_OK(index_builds::primary_driven::start(
        opCtx, nss.dbName(), collectionUUID, buildUUID, indexes, resumeStateIdent));

    persistPrimaryDrivenResumeState(
        opCtx, indexes, buildUUID, collectionUUID, resumeStateIdent, phase);

    indexBuildsCoord->onStepUp(opCtx);
    indexBuildsCoord->awaitStepUpThread_forTestOnly();
    indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(opCtx, collectionUUID);

    // The resume-state temp table must be dropped on commit. If options.isResumable was not set,
    // MultiIndexBlock::commit() skips the drop and the ident lingers.
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(opCtx, resumeStateIdent));
    {
        auto& resumeRu = *shard_role_details::getRecoveryUnit(opCtx);
        EXPECT_FALSE(storageEngine->getEngine()->hasIdent(resumeRu, resumeStateIdent));
    }

    std::vector<std::string> idents;
    for (auto& indexBuildInfo : indexes) {
        idents.push_back(indexBuildInfo.indexIdent);
    }

    auto collection = getCollectionExclusive(opCtx, nss);
    ASSERT_EQ(opObserver->startIndexBuildIdents, idents);
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        opCtx, opObserver->startIndexBuildIdents[0]));
    ASSERT(collection.getCollectionPtr()->getIndexCatalog()->findIndexByIdent(
        opCtx, opObserver->startIndexBuildIdents[1]));

    // Any phase before kDrainWrites includes a collection scan, which indexes the one document in
    // the collection as {a: null} → 1 key. kDrainWrites skips straight to draining side-writes → 0
    // keys.
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    const auto* indexEntry =
        collection.getCollectionPtr()->getIndexCatalog()->findIndexByName(opCtx, "a_1");
    ASSERT(indexEntry);
    const int64_t expectedKeys = (phase == IndexBuildPhaseEnum::kDrainWrites) ? 0 : 1;
    EXPECT_EQ(expectedKeys, indexEntry->accessMethod()->numKeys(opCtx, ru));
}

class IndexBuildsCoordinatorResumeOnStepUpTest
    : public IndexBuildsCoordinatorTest,
      public testing::WithParamInterface<IndexBuildPhaseEnum> {};

TEST_P(IndexBuildsCoordinatorResumeOnStepUpTest, StepUpResumesPrimaryDriven) {
    runResumePrimaryDrivenOnStepUpTest(
        operationContext(),
        storageInterface(),
        ::testing::UnitTest::GetInstance()->current_test_info()->name(),
        GetParam());
}

INSTANTIATE_TEST_SUITE_P(Phases,
                         IndexBuildsCoordinatorResumeOnStepUpTest,
                         testing::Values(IndexBuildPhaseEnum::kInitialized,
                                         IndexBuildPhaseEnum::kCollectionScan,
                                         IndexBuildPhaseEnum::kDrainWrites),
                         [](const testing::TestParamInfo<IndexBuildPhaseEnum>& info) {
                             switch (info.param) {
                                 case IndexBuildPhaseEnum::kInitialized:
                                     return "Initialized";
                                 case IndexBuildPhaseEnum::kCollectionScan:
                                     return "CollectionScan";
                                 case IndexBuildPhaseEnum::kDrainWrites:
                                     return "DrainWrites";
                                 default:
                                     MONGO_UNREACHABLE;
                             }
                         });

}  // namespace
}  // namespace mongo
