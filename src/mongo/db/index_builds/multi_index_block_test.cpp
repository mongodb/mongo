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
#include "mongo/db/index_builds/multi_index_block_gen.h"
#include "mongo/db/index_builds/resumable_index_builds_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
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
    indexer->abortWithoutCleanup(operationContext(), coll.get());
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

    indexer->setIsResumable(true);
    indexer->abortWithoutCleanup(operationContext(), coll.get());
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

    indexer->setIsResumable(true);
    indexer->persistResumeState(operationContext(), coll.get());
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

    indexer->setIsResumable(true);
    indexer->persistResumeState(operationContext(), coll.get());
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, autoColl->uuid());
    ASSERT_TRUE(indexBuildIdent);

    indexer->abortWithoutCleanup(operationContext(), coll.get());
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

TEST_F(MultiIndexBlockTest, CommitUsesCommitTimestampForTemporaryTableDrops) {
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
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    const Timestamp commitTs(100, 0);
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(commitTs);

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

    // The drop was registered at the commit timestamp. A timestamp <= commitTs should fail.
    ASSERT_THROWS_CODE(
        storageEngine->dropIdentTimestamped(operationContext(), sideWritesIdent, commitTs),
        DBException,
        ErrorCodes::ObjectIsBusy);

    // A timestamp greater than commitTs should succeed.
    storageEngine->dropIdentTimestamped(
        operationContext(), sideWritesIdent, Timestamp(commitTs.getSecs() + 1, 0));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

TEST_F(MultiIndexBlockTest, CommitWithNoCommitTimestampDropsImmediately) {
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

    // The ident is still in WiredTiger, pending drop in the reaper.
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    // Without a commit timestamp, the drop is registered as Immediate.
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

// With resumable primary-driven index builds enabled, MultiIndexBlock::init must create the
// per-build internal WT table whose ident matches ident::generateNewIndexBuildIdent(buildUUID),
// and MultiIndexBlock::commit must drop it.
TEST_F(MultiIndexBlockTest, CommitDropsResumablePrimaryDrivenIndexBuildTable) {
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest resumableEnabled{
        "featureFlagResumablePrimaryDrivenIndexBuilds", true};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

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

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }

    // After commit the ident is drop-pending; immediatelyCompletePendingDrop finalizes it.
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), indexBuildIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));
}

// When the build is not resumable, MultiIndexBlock::init must NOT eagerly create the per-build
// internal index build table — even if the container-write behavior is kReplicate.
TEST_F(MultiIndexBlockTest, InitSkipsResumablePrimaryDrivenIndexBuildTableWhenNotResumable) {
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    // Intentionally leave _isResumable at its default (false).

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

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// With resumable primary-driven index builds enabled, MultiIndexBlock::init must create the
// per-build internal WT table whose ident matches ident::generateNewIndexBuildIdent(buildUUID),
// and MultiIndexBlock::abort must drop it.
TEST_F(MultiIndexBlockTest, AbortDropsResumablePrimaryDrivenIndexBuildTable) {
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest resumableEnabled{
        "featureFlagResumablePrimaryDrivenIndexBuilds", true};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

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

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);

    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), indexBuildIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));
}

// With resumable PDIB enabled, the first call to drainBackgroundWrites must
// transition into kDrainWrites and persist a ResumeIndexInfo with that phase to the replicated
// internal-indexBuild-<UUID> table.
TEST_F(MultiIndexBlockTest, PdibPersistsResumeStateOnFirstDrain) {
    RAIIServerParameterControllerForTest containerWritesEnabled{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest resumableEnabled{
        "featureFlagResumablePrimaryDrivenIndexBuilds", true};

    // Container writes refuse if we're not the primary; the fixture's mock returns false from
    // canAcceptWritesFor by default, so allow writes explicitly.
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

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
    EXPECT_EQ(1U, specs.size());

    // Insert one document so insertAllDocumentsInCollection runs the collection scan and
    // creates the bulk loader for the kReplicate path.
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

    // Before any drain, the table exists but no resume record has been written yet.
    {
        auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
        if (storageEngine->getEngine()->hasIdent(
                *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent)) {
            shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
            EXPECT_FALSE(index_builds::readResumeIndexInfo(
                storageEngine, operationContext(), indexBuildIdent));
        }
    }

    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(IndexBuildPhaseEnum::kDrainWrites, resumeInfo->getPhase());
    EXPECT_EQ(autoColl->uuid(), resumeInfo->getCollectionUUID());

    // Re-entering drain (second/third pass in production) must not error and must leave the
    // record intact. Subsequent calls hit firstDrain == false and skip the persist.
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto laterResumeInfo = index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(laterResumeInfo);
    EXPECT_EQ(IndexBuildPhaseEnum::kDrainWrites, laterResumeInfo->getPhase());
    EXPECT_EQ(resumeInfo->getCollectionUUID(), laterResumeInfo->getCollectionUUID());

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, ResumePdibDuringDrain) {
    RAIIServerParameterControllerForTest containerWritesEnabled{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest resumableEnabled{
        "featureFlagResumablePrimaryDrivenIndexBuilds", true};

    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

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
    EXPECT_EQ(1U, specs.size());

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

    // Run the build through the drain phase so resume state is persisted with phase=kDrainWrites.
    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Insert a second document while the interceptor is still active so that it goes into the
    // side-writes table and is not yet drained. This entry must survive the simulated step-down
    // and be visible to the resumed build — verifying that init() reopens the existing table
    // (openExisting) rather than recreating it (immediate).
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 1 << "a" << 2)));
        wuow.commit();
    }

    // Count committed records in an internal table by iterating its cursor.
    auto countRecords = [&](StringData ident) -> size_t {
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        auto rs = storageEngine->getEngine()->getInternalRecordStore(ru, ident, KeyFormat::Long);
        auto cursor = rs->getCursor(operationContext(), ru);
        size_t count = 0;
        while (cursor->next()) {
            ++count;
        }
        return count;
    };

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    ASSERT_EQ(IndexBuildPhaseEnum::kDrainWrites, resumeInfo->getPhase());

    ASSERT_EQ(1U, resumeInfo->getIndexes().size());
    // All three ident fields must be propagated into the persisted resume state.
    EXPECT_EQ(*indexBuildInfo.sideWritesIdent, resumeInfo->getIndexes()[0].getSideWritesTable());
    ASSERT_TRUE(resumeInfo->getIndexes()[0].getSkippedRecordTrackerTable());
    EXPECT_EQ(*indexBuildInfo.skippedRecordsIdent,
              *resumeInfo->getIndexes()[0].getSkippedRecordTrackerTable());
    // storageIdentifier (sorter) is absent in kDrainWrites: the bulk load already completed.
    EXPECT_FALSE(resumeInfo->getIndexes()[0].getStorageIdentifier());

    // One pending side-writes entry exists before step-down.
    EXPECT_EQ(1u, countRecords(*indexBuildInfo.sideWritesIdent));

    // Simulate step-down: drop in-memory state without touching on-disk tables.
    indexer->markAsCleanedUp();

    // Simulate step-up resume: a new MultiIndexBlock for the same buildUUID initialized from the
    // persisted resume info.
    MultiIndexBlock resumedIndexer;
    resumedIndexer.setBuildUUID(buildUUID);
    resumedIndexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    resumedIndexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    resumedIndexer.setIsResumable(true);

    auto resumedSpecs =
        unittest::assertGet(resumedIndexer.init(operationContext(),
                                                coll,
                                                {indexBuildInfo},
                                                MultiIndexBlock::kNoopOnInitFn,
                                                MultiIndexBlock::InitMode::SteadyState,
                                                resumeInfo));
    EXPECT_EQ(1U, resumedSpecs.size());
    // The side-writes entry must still be present: init() must have reopened the existing table
    // (openExisting) rather than dropping and recreating it (immediate). If the table were
    // recreated, this count would be 0 and the pending write would be silently lost.
    EXPECT_EQ(1u, countRecords(*indexBuildInfo.sideWritesIdent));

    // drainBackgroundWrites also implicitly verifies that _phase was set from resumeInfo: the
    // function has an invariant requiring _phase == kBulkLoad || kDrainWrites, which would fire
    // if init() had left _phase at its default kInitialized.
    ASSERT_OK(
        resumedIndexer.drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    // All side-writes were drained by the resumed build.
    EXPECT_EQ(0u, countRecords(*indexBuildInfo.sideWritesIdent));

    resumedIndexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// Empty-collection PDIB short-circuits insertAllDocumentsInCollection before the bulk loader is
// initialized. Drain entry must skip the persist (rather than null-deref in _constructStateObject)
// and not leave a resume record on disk.
TEST_F(MultiIndexBlockTest, PdibSkipsResumeStateOnEmptyCollection) {
    RAIIServerParameterControllerForTest containerWritesEnabled{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest resumableEnabled{
        "featureFlagResumablePrimaryDrivenIndexBuilds", true};

    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

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
    EXPECT_EQ(1U, specs.size());

    // No documents inserted: insertAllDocumentsInCollection short-circuits, bulk loader is
    // never created.
    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    {
        auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
        if (storageEngine->getEngine()->hasIdent(
                *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent)) {
            shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
            EXPECT_FALSE(index_builds::readResumeIndexInfo(
                storageEngine, operationContext(), indexBuildIdent));
        }
    }

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// With the resumable PDIB feature flag disabled, drainBackgroundWrites does not persist
// resume info even on the PDIB path.
TEST_F(MultiIndexBlockTest, PdibDoesNotPersistResumeStateWhenFeatureFlagOff) {
    RAIIServerParameterControllerForTest pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest resumableDisabled{
        "featureFlagResumablePrimaryDrivenIndexBuilds", false};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    // When the resumable PDIB feature flag is off, production code uses kDoNotReplicate. We
    // mirror that here so init() takes the non-replicated path (no resume table created).
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kDoNotReplicate);

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
    EXPECT_EQ(1U, specs.size());

    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()),
        ident::generateNewIndexBuildIdent(buildUUID)));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
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

    indexer->setIsResumable(true);
    indexer->abortWithoutCleanup(operationContext(), coll.get());

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
TEST_F(MultiIndexBlockTest, AbortWithNoCommitTimestampDropsImmediately) {
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
                                                   boost::none,
                                                   /*generateTableWrites=*/true));
    ASSERT_EQUALS(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);

    // The ident is still in WiredTiger, pending drop in the reaper.
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    // Without a commit timestamp, the drop is registered as Immediate.
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    ASSERT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

// OpObserver that records integer-keyed onContainerInsert/onContainerUpdate calls so tests can
// verify that _writeStateToContainer went through the container_write path (which
// fires the observer).
class ResumeStateContainerInsertObserver : public OpObserverNoop {
public:
    using OpObserverNoop::onContainerInsert;
    using OpObserverNoop::onContainerUpdate;

    void onContainerInsert(OperationContext*,
                           StringData ident,
                           int64_t key,
                           std::span<const char> value) override {
        intInserts.push_back({std::string{ident}, key, std::string{value.begin(), value.end()}});
    }

    void onContainerUpdate(OperationContext*,
                           StringData ident,
                           int64_t key,
                           std::span<const char> value) override {
        intUpdates.push_back({std::string{ident}, key, std::string{value.begin(), value.end()}});
    }

    struct Op {
        std::string ident;
        int64_t key;
        std::string value;
    };
    std::vector<Op> intInserts;
    std::vector<Op> intUpdates;

    size_t countInsertsForIdent(StringData ident) const {
        return std::count_if(
            intInserts.begin(), intInserts.end(), [&](const Op& op) { return op.ident == ident; });
    }

    size_t countUpdatesForIdent(StringData ident) const {
        return std::count_if(
            intUpdates.begin(), intUpdates.end(), [&](const Op& op) { return op.ident == ident; });
    }
};

ResumeStateContainerInsertObserver& installResumeStateContainerObserver(OperationContext* opCtx) {
    auto observer = std::make_unique<ResumeStateContainerInsertObserver>();
    auto* ptr = observer.get();
    opCtx->getServiceContext()->resetOpObserver_forTest(std::move(observer));
    return *ptr;
}

// container_write::insert requires the node to be primary so the OpObserver fires for the
// container namespace.
void promoteMockReplCoordToPrimary(ServiceContext* service) {
    auto* replCoord =
        dynamic_cast<repl::ReplicationCoordinatorMock*>(repl::ReplicationCoordinator::get(service));
    ASSERT(replCoord);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
}

// State produced by setUpKReplicatePrimaryDrivenBuild. Owned by the test body so assertions can
// reference the input spec idents and the per-build resume-state ident.
struct KReplicateBuildHandle {
    UUID buildUUID;
    IndexBuildInfo indexBuildInfo;
    std::string indexBuildIdent;
};

// Setup for a primary-driven (kReplicate) build: assigns a build UUID, flips the container-write
// behavior, inserts one document into the collection (so the scan doesn't short-circuit on
// emptiness), constructs a single-spec IndexBuildInfo, runs init and the collection scan so
// index.bulk is populated for _constructStateObject. Caller still owns the AutoGetCollection /
// CollectionWriter and the feature-flag scope.
KReplicateBuildHandle setUpKReplicatePrimaryDrivenBuild(OperationContext* opCtx,
                                                        MultiIndexBlock* indexer,
                                                        AutoGetCollection& autoColl,
                                                        CollectionWriter& coll,
                                                        const NamespaceString& nss) {
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, *autoColl, BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    ASSERT_OK(indexer->init(opCtx,
                            coll,
                            {indexBuildInfo},
                            MultiIndexBlock::kNoopOnInitFn,
                            MultiIndexBlock::InitMode::SteadyState,
                            boost::none));
    ASSERT_OK(indexer->insertAllDocumentsInCollection(opCtx, nss));

    return {buildUUID, std::move(indexBuildInfo), ident::generateNewIndexBuildIdent(buildUUID)};
}

// persistResumeState for a primary-driven build with kReplicate goes through the container_write
// API: a single record is written at the fixed resume-state key, the OpObserver sees one
// onContainerInsert against the per-build ident, and the persisted document parses as
// ResumeIndexInfo with the expected fields.
TEST_F(MultiIndexBlockTest, PersistResumeStateUsesContainerWrites) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto handle =
        setUpKReplicatePrimaryDrivenBuild(operationContext(), indexer, autoColl, coll, getNSS());

    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    ASSERT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), handle.indexBuildIdent));

    // The load phase already produced one resume-state write; use a delta to isolate
    // what persistResumeState() contributes.
    const auto writesBefore = observer.countInsertsForIdent(handle.indexBuildIdent) +
        observer.countUpdatesForIdent(handle.indexBuildIdent);
    indexer->persistResumeState(operationContext(), coll.get());
    const auto writesAfter = observer.countInsertsForIdent(handle.indexBuildIdent) +
        observer.countUpdatesForIdent(handle.indexBuildIdent);

    ASSERT_EQUALS(writesBefore + 1, writesAfter);

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), handle.indexBuildIdent);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(handle.buildUUID, resumeInfo->getBuildUUID());
    EXPECT_EQ(autoColl->uuid(), resumeInfo->getCollectionUUID());

    // Per-index IndexStateInfo round-trips with the input spec / idents.
    EXPECT_EQ(1U, resumeInfo->getIndexes().size());
    const auto& indexState = resumeInfo->getIndexes()[0];
    EXPECT_EQ("a_1", indexState.getSpec()["name"].String());
    EXPECT_EQ(*handle.indexBuildInfo.sideWritesIdent, indexState.getSideWritesTable());

    // The observer's buffered BSONObj parses to the same buildUUID we read from the ident.
    auto resumeIt =
        std::find_if(observer.intInserts.begin(), observer.intInserts.end(), [&](const auto& ins) {
            return ins.ident == handle.indexBuildIdent;
        });
    ASSERT_NOT_EQUALS(resumeIt, observer.intInserts.end());
    EXPECT_EQ(
        handle.buildUUID,
        ResumeIndexInfo::parse(BSONObj(resumeIt->value.data()), IDLParserContext("ResumeIndexInfo"))
            .getBuildUUID());

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// Calling persistResumeState twice must overwrite the prior state at the same fixed key, so the
// table still contains exactly one record. This is what makes the container-write path safe
// without a separate truncate.
TEST_F(MultiIndexBlockTest, PersistResumeStateOverwritesPriorState) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto handle =
        setUpKReplicatePrimaryDrivenBuild(operationContext(), indexer, autoColl, coll, getNSS());

    // The load phase already produced one write; each persistResumeState() must add exactly one
    // more, and the second must be an update (key already exists after the first).
    const auto writesBefore = observer.countInsertsForIdent(handle.indexBuildIdent) +
        observer.countUpdatesForIdent(handle.indexBuildIdent);
    const auto updatesBefore = observer.countUpdatesForIdent(handle.indexBuildIdent);

    indexer->persistResumeState(operationContext(), coll.get());
    indexer->persistResumeState(operationContext(), coll.get());

    ASSERT_EQUALS(writesBefore + 2,
                  observer.countInsertsForIdent(handle.indexBuildIdent) +
                      observer.countUpdatesForIdent(handle.indexBuildIdent));
    ASSERT_GTE(observer.countUpdatesForIdent(handle.indexBuildIdent), updatesBefore + 1);

    // ...and the table still has a single record at the resume-state key (overwrite, not append).
    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), handle.indexBuildIdent);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(handle.buildUUID, resumeInfo->getBuildUUID());
    EXPECT_EQ(1U, resumeInfo->getIndexes().size());

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// abortWithoutCleanup persists the resume state via the container-write path for primary-driven
// builds, identical to persistResumeState.
TEST_F(MultiIndexBlockTest, AbortWithoutCleanupUsesContainerWrites) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto handle =
        setUpKReplicatePrimaryDrivenBuild(operationContext(), indexer, autoColl, coll, getNSS());

    // The load phase already produced one write; abortWithoutCleanup must add exactly one more.
    const auto writesBefore = observer.countInsertsForIdent(handle.indexBuildIdent) +
        observer.countUpdatesForIdent(handle.indexBuildIdent);
    indexer->abortWithoutCleanup(operationContext(), coll.get());
    ASSERT_EQUALS(writesBefore + 1,
                  observer.countInsertsForIdent(handle.indexBuildIdent) +
                      observer.countUpdatesForIdent(handle.indexBuildIdent));

    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), handle.indexBuildIdent);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(handle.buildUUID, resumeInfo->getBuildUUID());
    EXPECT_EQ(autoColl->uuid(), resumeInfo->getCollectionUUID());
    EXPECT_EQ(1U, resumeInfo->getIndexes().size());

    // After abortWithoutCleanup the per-build table must still exist (resumable shutdown).
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), handle.indexBuildIdent));
}

// persistResumeState is a no-op when the build is not resumable, regardless of method. The
// container-write path must not run.
TEST_F(MultiIndexBlockTest, PersistResumeStateNoOpWhenNotResumable) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto handle =
        setUpKReplicatePrimaryDrivenBuild(operationContext(), indexer, autoColl, coll, getNSS());
    indexer->setIsResumable(false);

    // The load phase already produced one write; with isResumable=false, persistResumeState
    // must be a no-op.
    const auto writesBefore = observer.countInsertsForIdent(handle.indexBuildIdent) +
        observer.countUpdatesForIdent(handle.indexBuildIdent);
    indexer->persistResumeState(operationContext(), coll.get());

    ASSERT_EQUALS(writesBefore,
                  observer.countInsertsForIdent(handle.indexBuildIdent) +
                      observer.countUpdatesForIdent(handle.indexBuildIdent));

    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    EXPECT_FALSE(index_builds::readResumeIndexInfo(
        storageEngine, operationContext(), handle.indexBuildIdent));


    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// When the build is hybrid (kDoNotReplicate), persistResumeState must not go through the
// container-write path even if the resumable PDIB feature flag is on.
TEST_F(MultiIndexBlockTest, HybridBuildDoesNotUseContainerWrites) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIsResumable(true);
    // No setContainerWriteBehavior — defaults to kDoNotReplicate (hybrid).

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto* storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    ASSERT_OK(indexer->init(operationContext(),
                            coll,
                            {indexBuildInfo},
                            MultiIndexBlock::kNoopOnInitFn,
                            MultiIndexBlock::InitMode::SteadyState,
                            boost::none));

    indexer->persistResumeState(operationContext(), coll.get());

    // Hybrid builds use the regular RecordStore path, so the OpObserver sees no container ops.
    ASSERT_EQUALS(0U, observer.intInserts.size());

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, WriteStateToContainerOnSpillWhenResumable) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    // Lower the per-build memory limit to 0.5 MB.
    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(0.5);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    // Insert enough indexable data to exceed the 1 MB memory limit. 20 documents with 64 KB strings
    // puts us comfortably above the limit.
    WriteUnitOfWork wuow(operationContext());
    std::string val(64 * 1024, 'a');
    for (auto i = 0; i < 20; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << i << "a" << val)));
    }
    wuow.commit();

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_TRUE(engine.getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    // Insert the data into the sorter. The first spill will trigger an insert into the index build
    // ident, and subsequent spills will trigger updates to the index build ident.
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));
    EXPECT_EQ(observer.countInsertsForIdent(indexBuildIdent), 1);
    EXPECT_GE(observer.countUpdatesForIdent(indexBuildIdent), 1);

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo =
        index_builds::readResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(resumeInfo.has_value());
    EXPECT_EQ(resumeInfo->getBuildUUID(), buildUUID);
    EXPECT_EQ(resumeInfo->getCollectionUUID(), autoColl->uuid());
    EXPECT_EQ(resumeInfo->getIndexes().size(), 1);
    EXPECT_EQ(resumeInfo->getIndexes()[0].getSpec()["name"].String(), "a_1");

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, DoNotWriteStateToContainerOnSpillWhenNotResumable) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};

    // Lower the per-build memory limit to 0.5 MB.
    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(0.5);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(false);

    // Insert enough indexable data to exceed the 1 MB memory limit. 20 documents with 64 KB strings
    // puts us comfortably above the limit.
    WriteUnitOfWork wuow(operationContext());
    std::string val(64 * 1024, 'a');
    for (auto i = 0; i < 20; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << i << "a" << val)));
    }
    wuow.commit();

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    EXPECT_FALSE(engine.getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    // Insert the data into the sorter. The first spill will trigger an insert into the index build
    // ident, and subsequent spills will trigger updates to the index build ident.
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));
    EXPECT_EQ(observer.countInsertsForIdent(indexBuildIdent), 0);
    EXPECT_GE(observer.countUpdatesForIdent(indexBuildIdent), 0);

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}
TEST_F(MultiIndexBlockTest, LoadWritesResumeStatePeriodicallyForPrimaryDrivenBuild) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};
    // Force frequent batch commits and resume-state writes during the load phase.
    RAIIServerParameterControllerForTest insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    RAIIServerParameterControllerForTest resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 10};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    WriteUnitOfWork wuow(operationContext());
    for (auto i = 0; i < 50; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << i << "a" << i)));
    }
    wuow.commit();

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    // 50 keys with interval=10 deterministically produces 5 periodic writes (one per 10
    // committed keys, including the final batch).
    auto opsObserved = observer.countInsertsForIdent(indexBuildIdent) +
        observer.countUpdatesForIdent(indexBuildIdent);
    EXPECT_EQ(opsObserved, 5U);

    // Each captured periodic write must parse as a valid ResumeIndexInfo originating from the
    // load phase (kBulkLoad), proving the writes really come from this code path and not from
    // init / collection-scan.
    std::vector<ResumeIndexInfo> persistedStates;
    for (const auto& op : observer.intInserts) {
        if (op.ident == indexBuildIdent) {
            BSONObj obj(op.value.data());
            persistedStates.push_back(
                ResumeIndexInfo::parse(obj, IDLParserContext("ResumeIndexInfo")));
        }
    }
    for (const auto& op : observer.intUpdates) {
        if (op.ident == indexBuildIdent) {
            BSONObj obj(op.value.data());
            persistedStates.push_back(
                ResumeIndexInfo::parse(obj, IDLParserContext("ResumeIndexInfo")));
        }
    }
    EXPECT_EQ(persistedStates.size(), 5U);
    for (const auto& info : persistedStates) {
        EXPECT_EQ(info.getPhase(), IndexBuildPhaseEnum::kBulkLoad);
        EXPECT_EQ(info.getBuildUUID(), buildUUID);
        EXPECT_EQ(info.getIndexes().size(), 1U);
    }

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, LoadDoesNotPeriodicallyWriteWhenNotResumable) {
    RAIIServerParameterControllerForTest ffContainerWrites{"featureFlagContainerWrites", true};
    RAIIServerParameterControllerForTest ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    RAIIServerParameterControllerForTest ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     true};
    RAIIServerParameterControllerForTest insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    RAIIServerParameterControllerForTest resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 10};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(false);

    WriteUnitOfWork wuow(operationContext());
    for (auto i = 0; i < 50; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << i << "a" << i)));
    }
    wuow.commit();

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto opsObserved = observer.countInsertsForIdent(indexBuildIdent) +
        observer.countUpdatesForIdent(indexBuildIdent);
    EXPECT_EQ(0U, opsObserved);

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}
}  // namespace
}  // namespace mongo
