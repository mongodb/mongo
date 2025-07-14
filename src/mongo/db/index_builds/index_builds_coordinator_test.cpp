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
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/op_observer/op_observer_noop.h"
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
        ASSERT_OK(collection_internal::insertDocument(
            opCtx, *collection, InsertStatement(BSON("_id" << i << "a" << 1)), nullOpDebug));
        wuow.commit();
    }

    ASSERT_EQ(collection->getIndexCatalog()->numIndexesTotal(), 1);
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

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    ASSERT_THROWS_CODE(indexBuildsCoord->createIndex(
                           opCtx, collection->uuid(), spec, indexConstraints, fromMigrate),
                       AssertionException,
                       ErrorCodes::DuplicateKey);
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 1);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundUniqueRelax) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundUniqueRelax");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    auto fromMigrate = false;
    ASSERT_DOES_NOT_THROW(indexBuildsCoord->createIndex(
        opCtx, collection->uuid(), spec, indexConstraints, fromMigrate));
    ASSERT_EQ(coll(opCtx, nss)->getIndexCatalog()->numIndexesTotal(), 2);
}

TEST_F(IndexBuildsCoordinatorTest, ForegroundIndexAlreadyExists) {
    auto opCtx = operationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.ForegroundIndexAlreadyExists");
    createCollectionWithDuplicateDocs(opCtx, nss);

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                         << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    auto uuid = collection->uuid();
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

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec1 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto spec2 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_2"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    auto uuid = collection->uuid();
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

    AutoGetCollection collection(opCtx, nss, MODE_X);
    ASSERT(collection);
    auto indexKey = BSON("a" << 1);
    auto spec1 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_1"));
    auto spec2 = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                          << (indexKey.firstElementFieldNameStringData() + "_2"));
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    auto fromMigrate = false;
    auto uuid = collection->uuid();
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
                       BSONObj indexDoc,
                       StringData ident,
                       bool fromMigrate) override {
        createIndexIdents.emplace_back(ident);
    }

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           const std::vector<std::string>& idents,
                           bool fromMigrate) override {
        startIndexBuildIdents = idents;
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
        AutoGetCollection autoColl(operationContext(), nss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), autoColl};
        IndexBuildsCoordinator::createIndexesOnEmptyCollection(
            operationContext(),
            writer,
            {BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1")},
            false);
        wuow.commit();
    }

    // Verify that the op observer was called and that it was given the correct ident
    AutoGetCollection autoColl(operationContext(), nss, MODE_X);
    ASSERT_EQ(opObserver->createIndexIdents.size(), 1);
    ASSERT(autoColl->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                         opObserver->createIndexIdents[0]));
}

TEST_F(IndexBuildsCoordinatorTest, CreateIndexOnNonEmptyCollectionReplicatesIdent) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    {
        AutoGetCollection autoColl(operationContext(), nss, MODE_X);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), *autoColl, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullptr));
        wuow.commit();

        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        indexBuildsCoord->createIndex(operationContext(),
                                      autoColl->uuid(),
                                      BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                                      IndexBuildsManager::IndexConstraints::kRelax,
                                      false);
    }

    // Verify that the op observer was called and that it was given the correct ident
    AutoGetCollection autoColl(operationContext(), nss, MODE_X);
    ASSERT_EQ(opObserver->createIndexIdents.size(), 1);
    ASSERT(autoColl->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                         opObserver->createIndexIdents[0]));
}

TEST_F(IndexBuildsCoordinatorTest, StartIndexBuildOnEmptyCollectionReplicatesAsCreateIndex) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    std::vector<BSONObj> specs = {BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                                  BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1")};
    std::vector<std::string> idents = {"index-1", "index-2"};

    {
        AutoGetCollection autoColl(operationContext(), nss, MODE_X);
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto status = indexBuildsCoord->startIndexBuild(operationContext(),
                                                        nss.dbName(),
                                                        autoColl->uuid(),
                                                        specs,
                                                        idents,
                                                        UUID::gen(),
                                                        IndexBuildProtocol::kTwoPhase,
                                                        {});
        ASSERT_OK(status);
        status.getValue().wait();
    }

    // Verify that the op observer was called, that it was given the expected idents, and that the
    // specified idents were actually used. Since the collection was empty, onCreateIndex() was
    // called instead of onStartIndexBuild.
    AutoGetCollection autoColl(operationContext(), nss, MODE_X);
    ASSERT_EQ(opObserver->createIndexIdents, idents);
    ASSERT(autoColl->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                         opObserver->createIndexIdents[0]));
    ASSERT(autoColl->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                         opObserver->createIndexIdents[1]));
}

TEST_F(IndexBuildsCoordinatorTest, StartIndexBuildOnNonEmptyCollectionReplicatesIdents) {
    auto opObserver = OpObserverMock::install(operationContext());

    const auto nss = NamespaceString::createNamespaceString_forTest(
        "IndexBuildsCoordinatorTest.CreateIndexOnEmptyCollectionReplicatesIdent");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), NamespaceString::kIndexBuildEntryNamespace, CollectionOptions()));

    std::vector<BSONObj> specs = {BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                                  BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << "b_1")};
    std::vector<std::string> idents = {"index-1", "index-2"};

    auto collUUID = [&] {
        AutoGetCollection autoColl(operationContext(), nss, MODE_X);

        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), *autoColl, InsertStatement(BSON("_id" << 1 << "a" << 1)), nullptr));
        wuow.commit();

        return autoColl->uuid();
    }();

    {
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto buildUUID = UUID::gen();
        auto buildFuture =
            unittest::assertGet(indexBuildsCoord->startIndexBuild(operationContext(),
                                                                  nss.dbName(),
                                                                  collUUID,
                                                                  specs,
                                                                  idents,
                                                                  buildUUID,
                                                                  IndexBuildProtocol::kTwoPhase,
                                                                  {CommitQuorumOptions(1)}));
        ASSERT_OK(indexBuildsCoord->voteCommitIndexBuild(
            operationContext(), buildUUID, HostAndPort("test1", 1234)));
        ASSERT_OK(buildFuture.getNoThrow());
    }

    // Verify that the op observer was called, that it was given the expected idents, and that the
    // specified idents were actually used.
    AutoGetCollection autoColl(operationContext(), nss, MODE_X);
    ASSERT_EQ(opObserver->startIndexBuildIdents, idents);
    ASSERT(autoColl->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                         opObserver->startIndexBuildIdents[0]));
    ASSERT(autoColl->getIndexCatalog()->findIndexByIdent(operationContext(),
                                                         opObserver->startIndexBuildIdents[1]));
}

}  // namespace
}  // namespace mongo
