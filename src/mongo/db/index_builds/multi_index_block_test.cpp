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
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
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
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

        std::unique_ptr<RecordStore> rs = storageEngine->getEngine()->getInternalRecordStore(
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
                                                   boost::none));
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
                                                   boost::none));
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
                                                   boost::none));
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
    indexer->setBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo1},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none));
    ASSERT_EQUALS(1U, specs.size());

    auto isResumable = true;
    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(indexBuildIdent);

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *indexBuildIdent});
}

TEST_F(MultiIndexBlockTest, PersistResumeStateOnRequestAndCommit) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo1},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none));
    ASSERT_EQUALS(1U, specs.size());

    auto isResumable = true;
    indexer->persistResumeState(operationContext(), coll.get(), isResumable);
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(indexBuildIdent);

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *indexBuildIdent},
                                      /*droppable=*/true);
}

TEST_F(MultiIndexBlockTest, PersistResumeStateOnRequestAndOnAbort) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo1},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none));
    ASSERT_EQUALS(1U, specs.size());

    auto isResumable = true;
    indexer->persistResumeState(operationContext(), coll.get(), isResumable);
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(indexBuildIdent);

    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);
    indexBuildIdent = findPersistedResumeState(operationContext(), buildUUID);
    ASSERT_TRUE(indexBuildIdent);

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *indexBuildIdent});
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
                       getNSS().dbName());

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_THROWS_CODE(
            indexer->init(
                operationContext(),
                coll,
                {indexBuildInfo},
                [] { throwWriteConflictException("Throw WriteConflictException in 'OnInitFn'."); },
                MultiIndexBlock::InitMode::SteadyState,
                boost::none),
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
                             boost::none)
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
                       "index-1",
                       *storageEngine);
    auto indexBuildInfo2 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-2",
                       *storageEngine);

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
                                 boost::none)
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_NE(status, ErrorCodes::IndexBuildAlreadyInProgress);
    }

    // Start one index build is OK
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo1},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none)
                      .getStatus());
        wuow.commit();
    }

    auto secondaryIndexer = std::make_unique<MultiIndexBlock>();

    // Trying to start the index build again fails with IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo1},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none)
                      .getStatus(),
                  ErrorCodes::IndexBuildAlreadyInProgress);
    }

    // Trying to start multiple index builds with the same spec fails with
    // IndexBuildAlreadyInProgress if there is an existing index build matching any spec
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo1, indexBuildInfo2},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none)
                      .getStatus(),
                  ErrorCodes::IndexBuildAlreadyInProgress);
    }

    auto indexBuildInfo3 =
        IndexBuildInfo(BSON("key" << BSON("b" << 1) << "name"
                                  << "b_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-2",
                       *storageEngine);

    // If one of the requested specs are already in progress we fail with
    // IndexBuildAlreadyInProgress
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_EQ(secondaryIndexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo3, indexBuildInfo1},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none)
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
                       "index-1",
                       *storageEngine);

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer
                      ->init(operationContext(),
                             coll,
                             {indexBuildInfo},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none)
                      .getStatus());
        wuow.commit();
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 0 << "a" << 1)));
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

TEST_F(MultiIndexBlockTest, DrainBackgroundWritesYieldIsTracked) {
    otel::metrics::OtelMetricsCapturer capturer;
    int64_t drainYieldsBefore = 0;
    if (capturer.canReadMetrics()) {
        drainYieldsBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDrainYields);
    }

    auto indexer = getIndexer();
    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    {
        AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
        CollectionWriter coll(operationContext(), autoColl);

        {
            WriteUnitOfWork wuow(operationContext());
            ASSERT_OK(indexer
                          ->init(operationContext(),
                                 coll,
                                 {indexBuildInfo},
                                 MultiIndexBlock::kNoopOnInitFn,
                                 MultiIndexBlock::InitMode::SteadyState,
                                 boost::none)
                          .getStatus());
            wuow.commit();
        }

        {
            WriteUnitOfWork wuow(operationContext());
            ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 0 << "a" << 1)));
            wuow.commit();
        }

        ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    }

    {
        AutoGetCollection autoColl(operationContext(), getNSS(), MODE_IX);
        ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                                 RecoveryUnit::ReadSource::kNoTimestamp,
                                                 IndexBuildInterceptor::DrainYieldPolicy::kYield));
    }

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDrainYields),
            drainYieldsBefore + 1);
    }

    {
        AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
        CollectionWriter coll(operationContext(), autoColl);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }
}

TEST_F(MultiIndexBlockTest, CommitDropsTemporaryTables) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none));
    ASSERT_EQUALS(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }

    // After commit, the ident is still in WiredTiger but is drop-pending in the reaper.
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

TEST_F(MultiIndexBlockTest, AbortDropsTemporaryTables) {
    auto indexer = getIndexer();

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none));
    ASSERT_EQUALS(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);

    // After abort, the ident is still in WiredTiger but is drop-pending in the reaper.
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

TEST_F(MultiIndexBlockTest, AbortWithoutCleanupDoesNotDropTables) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {indexBuildInfo},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::SteadyState,
                                                   boost::none));
    ASSERT_EQUALS(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;

    auto isResumable = true;
    indexer->abortWithoutCleanup(operationContext(), coll.get(), isResumable);

    // After abortWithoutCleanup, the ident should still exist and not be drop-pending.
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

TEST_F(MultiIndexBlockTest, BasicMetrics) {
    // Test that an index build with two specs generates proper metrics. Shou
    auto indexer = getIndexer();

    otel::metrics::OtelMetricsCapturer capturer;
    int64_t scannedBefore = 0;
    int64_t keysGeneratedBefore = 0;
    int64_t keysInsertedBefore = 0;
    if (capturer.canReadMetrics()) {
        scannedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildDocsScanned);
        keysGeneratedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildKeysGeneratedFromScan);
        keysInsertedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildKeysInsertedFromScan);
    }

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto spec1 =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);
    auto spec2 =
        IndexBuildInfo(BSON("key" << BSON("b" << 1) << "name"
                                  << "b_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-2",
                       *storageEngine);
    constexpr size_t numIndexSpecs = 2;

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer
                      ->init(operationContext(),
                             coll,
                             {spec1, spec2},
                             MultiIndexBlock::kNoopOnInitFn,
                             MultiIndexBlock::InitMode::SteadyState,
                             boost::none)
                      .getStatus());
        wuow.commit();
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 0 << "a" << 1)));
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 1 << "a" << 2)));
        wuow.commit();
    }
    constexpr size_t numDocsInColl = 2;

    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildDocsScanned),
                  scannedBefore + numDocsInColl);
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildKeysGeneratedFromScan),
            keysGeneratedBefore + (numDocsInColl * numIndexSpecs));
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildKeysInsertedFromScan),
            keysInsertedBefore + (numDocsInColl * numIndexSpecs));
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }

    // Same validation as before commit().
    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildDocsScanned),
                  scannedBefore + numDocsInColl);
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildKeysGeneratedFromScan),
            keysGeneratedBefore + (numDocsInColl * numIndexSpecs));
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildKeysInsertedFromScan),
            keysInsertedBefore + (numDocsInColl * numIndexSpecs));
    }
}
}  // namespace
}  // namespace mongo
