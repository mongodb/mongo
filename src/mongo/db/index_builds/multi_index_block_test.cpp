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

#include "mongo/db/index_builds/multi_index_block.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

/**
 * Unit test for MultiIndexBlock to verify basic functionality.
 */
class MultiIndexBlockTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

protected:
    NamespaceString getNSS() const {
        return _nss;
    }

    MultiIndexBlock* getIndexer() const {
        return _indexer.get();
    }

    // On teardown, asserts that the temporary tables for all given idents exist and that these
    // temporary tables can be dropped if expected.
    void validateTempTableIdentsOnTeardown(const std::vector<std::string>& idents,
                                           bool droppable = false) {
        _tempTableIdentsToValidate = idents;
        _tempTableIdentsDroppable = droppable;
    }

private:
    NamespaceString _nss;
    std::unique_ptr<MultiIndexBlock> _indexer;
    std::vector<std::string> _tempTableIdentsToValidate;
    bool _tempTableIdentsDroppable;
};

void MultiIndexBlockTest::setUp() {
    CatalogTestFixture::setUp();

    auto service = getServiceContext();
    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    _nss = NamespaceString::createNamespaceString_forTest("db.coll");

    CollectionOptions options;
    options.uuid = UUID::gen();

    ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, options));
    _indexer = std::make_unique<MultiIndexBlock>();
}

void MultiIndexBlockTest::tearDown() {
    _indexer = {};

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    for (const auto& ident : _tempTableIdentsToValidate) {
        if (!storageEngine->getEngine()->hasIdent(
                *shard_role_details::getRecoveryUnit(operationContext()), ident)) {
            FAIL(std::string(str::stream() << "Expected storage engine to have ident: " << ident));
        };

        if (_tempTableIdentsDroppable) {
            ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), ident));
            if (storageEngine->getEngine()->hasIdent(
                    *shard_role_details::getRecoveryUnit(operationContext()), ident)) {
                FAIL(std::string(str::stream()
                                 << "Expected storage engine to have dropped ident: " << ident));
            }
        };
    }

    CatalogTestFixture::tearDown();
}

// Check if there is a resumable index build temporary table with a persisted resume state that
// corresponds to the given index build UUID. If one matches, the ident is returned; if there are
// multiple matches or persisted states in the temporary table, this will assert.
boost::optional<std::string> findPersistedResumeState(
    OperationContext* opCtx,
    const UUID& buildUUID,
    boost::optional<IndexBuildPhaseEnum> phase = boost::none,
    boost::optional<UUID> collectionUUID = boost::none) {
    boost::optional<std::string> foundResumeIdent;
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const auto idents =
        storageEngine->getEngine()->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx));

    for (auto it = idents.begin(); it != idents.end(); ++it) {
        if (!ident::isInternalIdent(*it, kResumableIndexIdentStem)) {
            continue;
        }

        std::unique_ptr<RecordStore> rs = storageEngine->getEngine()->getTemporaryRecordStore(
            *shard_role_details::getRecoveryUnit(opCtx), *it, KeyFormat::Long);
        auto cursor = rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        auto record = cursor->next();
        if (!record) {
            continue;
        }

        auto doc = record->data.toBson();
        ResumeIndexInfo resumeInfo;
        try {
            resumeInfo = ResumeIndexInfo::parse(doc, IDLParserContext("ResumeIndexInfo"));
        } catch (const DBException&) {
            continue;
        }

        if (resumeInfo.getBuildUUID() != buildUUID) {
            continue;
        }

        if (phase) {
            ASSERT_EQUALS(*phase, resumeInfo.getPhase());
        }

        if (collectionUUID) {
            ASSERT_EQUALS(*collectionUUID, resumeInfo.getCollectionUUID());
        }

        // Should not discover >1 recovery ident with a resumeInfo containing the same buildUUID, or
        // multiple documents in the recovery ident.
        ASSERT_FALSE(foundResumeIdent);
        ASSERT_FALSE(cursor->next());

        foundResumeIdent = *it;
    }

    return foundResumeIdent;
}

TEST_F(MultiIndexBlockTest, CommitWithoutInsertingDocuments) {
    auto indexer = getIndexer();

    auto coll = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest(getNSS(),
                                     PlacementConcern::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(operationContext()),
                                     AcquisitionPrerequisites::kWrite),
        MODE_X);
    CollectionWriter collWriter(operationContext(), &coll);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   collWriter,
                                                   {},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(0U, specs.size());

    ASSERT_OK(indexer->dumpInsertsFromBulk(operationContext(), coll));
    ASSERT_OK(indexer->checkConstraints(operationContext(), coll.getCollectionPtr()));

    {
        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  collWriter.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wunit.commit();
    }
}

TEST_F(MultiIndexBlockTest, CommitAfterInsertingSingleDocument) {
    auto indexer = getIndexer();

    auto coll = acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest(getNSS(),
                                     PlacementConcern::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(operationContext()),
                                     AcquisitionPrerequisites::kWrite),
        MODE_X);
    CollectionWriter collWriter(operationContext(), &coll);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   collWriter,
                                                   {},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::InitialSync,
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(0U, specs.size());

    ASSERT_OK(indexer->insertSingleDocumentForInitialSyncOrRecovery(
        operationContext(),
        coll.getCollectionPtr(),
        {},
        {},
        /*saveCursorBeforeWrite*/ []() {},
        /*restoreCursorAfterWrite*/ []() {}));
    ASSERT_OK(indexer->dumpInsertsFromBulk(operationContext(), coll));
    ASSERT_OK(indexer->checkConstraints(operationContext(), coll.getCollectionPtr()));

    {
        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  collWriter.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wunit.commit();
    }

    // abort() should have no effect after the index build is committed.
    indexer->abortIndexBuild(operationContext(), collWriter, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, AbortWithoutCleanupAfterInsertingSingleDocument) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::InitialSync,
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(0U, specs.size());
    ASSERT_OK(indexer->insertSingleDocumentForInitialSyncOrRecovery(
        operationContext(),
        coll.get(),
        {},
        {},
        /*saveCursorBeforeWrite*/ []() {},
        /*restoreCursorAfterWrite*/ []() {}));
    auto isResumable = false;
    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);
}

TEST_F(MultiIndexBlockTest, PersistResumeStateOnAbortWithoutCleanup) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setTwoPhaseBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-1"});
    indexBuildInfo1.setInternalIdents(*storageEngine,
                                      VersionContext::getDecoration(operationContext()));

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo1},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(1U, specs.size());

    auto isResumable = true;
    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);
    auto resumeIndexIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(resumeIndexIdent);

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *resumeIndexIdent});
}

TEST_F(MultiIndexBlockTest, PersistResumeStateOnRequestAndCommit) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setTwoPhaseBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-1"});
    indexBuildInfo1.setInternalIdents(*storageEngine,
                                      VersionContext::getDecoration(operationContext()));

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo1},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(1U, specs.size());

    auto isResumable = true;
    indexer->persistResumeState(operationContext(), coll.get(), isResumable);
    auto resumeIndexIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(resumeIndexIdent);

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *resumeIndexIdent},
                                      /*droppable=*/true);
}

TEST_F(MultiIndexBlockTest, PersistResumeStateOnRequestAndOnAbort) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setTwoPhaseBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-1"});
    indexBuildInfo1.setInternalIdents(*storageEngine,
                                      VersionContext::getDecoration(operationContext()));

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo1},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(1U, specs.size());

    auto isResumable = true;
    indexer->persistResumeState(operationContext(), coll.get(), isResumable);
    auto resumeIndexIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(resumeIndexIdent);

    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);
    resumeIndexIdent = findPersistedResumeState(operationContext(), buildUUID);
    ASSERT_TRUE(resumeIndexIdent);

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *resumeIndexIdent});
}

TEST_F(MultiIndexBlockTest, InitWriteConflictException) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       *storageEngine,
                       getNSS().dbName(),
                       VersionContext::getDecoration(operationContext()));

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_THROWS_CODE(
            indexer->init(
                operationContext(),
                coll,
                {indexBuildInfo},
                [] { throwWriteConflictException("Throw WriteConflictException in 'OnInitFn'."); },
                MultiIndexBlock::InitMode::SteadyState,
                boost::none,
                /*generateTableWrites=*/true),
            DBException,
            ErrorCodes::WriteConflict);
    }

    {
        WriteUnitOfWork wuow(operationContext());
        indexBuildInfo.indexIdent = "index-1";
        ASSERT_OK(indexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none,
                             /*generateTableWrites=*/true)
                      .getStatus());
        wuow.commit();
    }

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, InitMultipleSpecs) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-1"});
    indexBuildInfo1.setInternalIdents(*storageEngine,
                                      VersionContext::getDecoration(operationContext()));
    auto indexBuildInfo2 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-2"});
    indexBuildInfo2.setInternalIdents(*storageEngine,
                                      VersionContext::getDecoration(operationContext()));

    // Starting multiple index builds that conflicts with each other fails, but not with
    // IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        auto status = indexer
                          ->init(operationContext(),
                                 coll,
                                 {indexBuildInfo1, indexBuildInfo2},
                                 MultiIndexBlock::kNoopOnInitFn,
                                 MultiIndexBlock::InitMode::SteadyState,
                                 boost::none,
                                 /*generateTableWrites=*/true)
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_NE(status, ErrorCodes::IndexBuildAlreadyInProgress);
    }

    // Start one index build is OK
    {
        WriteUnitOfWork wuow(operationContext());
        indexBuildInfo1.setInternalIdents(*storageEngine,
                                          VersionContext::getDecoration(operationContext()));
        ASSERT_OK(indexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo1},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none,
                             /*generateTableWrites=*/true)
                      .getStatus());
        wuow.commit();
    }

    auto secondaryIndexer = std::make_unique<MultiIndexBlock>();

    // Trying to start the index build again fails with IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        indexBuildInfo1.setInternalIdents(*storageEngine,
                                          VersionContext::getDecoration(operationContext()));
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo1},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none,
                             /*generateTableWrites=*/true)
                      .getStatus(),
                  ErrorCodes::IndexBuildAlreadyInProgress);
    }

    // Trying to start multiple index builds with the same spec fails with
    // IndexBuildAlreadyInProgress if there is an existing index build matching any spec
    {
        WriteUnitOfWork wuow(operationContext());
        indexBuildInfo1.setInternalIdents(*storageEngine,
                                          VersionContext::getDecoration(operationContext()));
        indexBuildInfo2.setInternalIdents(*storageEngine,
                                          VersionContext::getDecoration(operationContext()));
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo1, indexBuildInfo2},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none,
                             /*generateTableWrites=*/true)
                      .getStatus(),
                  ErrorCodes::IndexBuildAlreadyInProgress);
    }

    auto indexBuildInfo3 =
        IndexBuildInfo(BSON("key" << BSON("b" << 1) << "name"
                                  << "b_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-2"});
    indexBuildInfo3.setInternalIdents(*storageEngine,
                                      VersionContext::getDecoration(operationContext()));

    // If one of the requested specs are already in progress we fail with
    // IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        indexBuildInfo1.setInternalIdents(*storageEngine,
                                          VersionContext::getDecoration(operationContext()));
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo3, indexBuildInfo1},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none,
                             /*generateTableWrites=*/true)
                      .getStatus(),
                  ErrorCodes::IndexBuildAlreadyInProgress);
    }

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, AddDocumentBetweenInitAndInsertAll) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       std::string{"index-1"});
    indexBuildInfo.setInternalIdents(*storageEngine,
                                     VersionContext::getDecoration(operationContext()));

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none,
                             /*generateTableWrites=*/true)
                      .getStatus());
        wuow.commit();
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), *autoColl, InsertStatement(BSON("_id" << 0 << "a" << 1)), nullptr));
        wuow.commit();
    }

    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }
}

}  // namespace
}  // namespace mongo
