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
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/preallocated_container_pool.h"
#include "mongo/db/index_builds/multi_index_block_gen.h"
#include "mongo/db/index_builds/repl_index_build_state.h"
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
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <string_view>
#include <variant>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

/**
 * Unit test for MultiIndexBlock to verify basic functionality.
 */
class MultiIndexBlockTest : public CatalogTestFixture {
protected:
    void setUp() override;
    void tearDown() override;

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

// State produced by setUpKReplicatePrimaryDrivenBuild. Owned by the test body so assertions can
// reference the input spec idents and the per-build resume-state ident.
struct KReplicateBuildHandle {
    UUID buildUUID;
    IndexBuildInfo indexBuildInfo;
    std::string indexBuildIdent;
};

KReplicateBuildHandle setUpKReplicatePrimaryDrivenBuild(OperationContext* opCtx,
                                                        MultiIndexBlock* indexer,
                                                        CollectionAcquisition& acq,
                                                        CollectionWriter& coll,
                                                        const NamespaceString& nss);

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
            EXPECT_EQ(*phase, resumeInfo.getPhase());
        }

        if (collectionUUID) {
            EXPECT_EQ(*collectionUUID, resumeInfo.getCollectionUUID());
        }

        // Should not discover >1 recovery ident with a resumeInfo containing the same buildUUID, or
        // multiple documents in the recovery ident.
        EXPECT_FALSE(foundResumeIdent);
        EXPECT_FALSE(cursor->next());

        foundResumeIdent = *it;
    }

    return foundResumeIdent;
}

// Count the number of records stored in the internal record store identified by `ident`.
size_t countRecordsInIdent(OperationContext* opCtx,
                           StorageEngine* storageEngine,
                           std::string_view ident) {
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto rs = storageEngine->getEngine()->getInternalRecordStore(ru, ident, KeyFormat::Long);
    auto cursor = rs->getCursor(opCtx, ru);
    size_t count = 0;
    while (cursor->next())
        ++count;
    return count;
}

class MultiIndexBlockResumableTest : public MultiIndexBlockTest {
private:
    void setUp() override {
        MultiIndexBlockTest::setUp();
        _containerWritesEnabled.emplace("featureFlagContainerWrites", true);
        _pdibEnabled.emplace("featureFlagPrimaryDrivenIndexBuilds", true);
        _resumableEnabled.emplace("featureFlagResumablePrimaryDrivenIndexBuilds", true);
        static_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(getServiceContext()))
            ->alwaysAllowWrites(true);
    }

    boost::optional<unittest::ServerParameterGuard> _containerWritesEnabled;
    boost::optional<unittest::ServerParameterGuard> _pdibEnabled;
    boost::optional<unittest::ServerParameterGuard> _resumableEnabled;
};

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
    EXPECT_EQ(0U, specs.size());

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
    EXPECT_EQ(0U, specs.size());

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

// The bulk commit of a unique index wraps IndexBuildInterceptor::recordDuplicateKey in a
// writeConflictRetry block. Verify that a storage engine write conflict on that write is retried
// and that the duplicate is persisted exactly once in the constraint-violations ident.
TEST_F(MultiIndexBlockResumableTest, DuplicateKeyRecordingSurvivesWriteConflict) {
    auto* opCtx = operationContext();
    // Batch size >1 keeps both keys in the same in-memory batch, so the recordDuplicateKey insert
    // is the first time the WCE will hit.
    unittest::ServerParameterGuard insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    auto indexer = getIndexer();
    indexer->setBuildUUID(UUID::gen());
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    // isResumable=false avoids periodic _writeStateToContainer callbacks during the load phase
    // that could fire on the armed WCE before the duplicate-recording write.
    indexer->setIsResumable(false);

    AutoGetCollection autoColl(opCtx, getNSS(), MODE_X);
    CollectionWriter coll(opCtx, autoColl);

    // Two records with identical `a` values: after sorting in the bulk loader they are adjacent
    // duplicates, which triggers onDuplicateKeyInserted on the second key.
    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, *autoColl, BSON("_id" << 0 << "a" << 1)));
        ASSERT_OK(Helpers::insert(opCtx, *autoColl, BSON("_id" << 1 << "a" << 1)));
        wuow.commit();
    }

    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    IndexBuildInfo indexBuildInfo(BSON("v" << static_cast<int>(IndexConfig::kLatestIndexVersion)
                                           << "key" << BSON("a" << 1) << "name"
                                           << "a_1"
                                           << "unique" << true),
                                  "index-1",
                                  *storageEngine);
    ASSERT_TRUE(indexBuildInfo.constraintViolationsIdent);

    ASSERT_OK(indexer
                  ->init(opCtx,
                         coll,
                         {indexBuildInfo},
                         MultiIndexBlock::kNoopOnInitFn,
                         MultiIndexBlock::InitMode::SteadyState,
                         boost::none)
                  .getStatus());

    // Arm a WCE to fire exactly once. With two in-memory docs the sorter does not
    // spill during the scan, and with kReplicate the bulk commit accumulates both keys into one
    // batch before flushing, so the first write reaching the WCE check is the recordDuplicateKey
    // insert inside writeConflictRetry("recordingDuplicateKey").
    {
        auto failPoint = enableWriteConflictForWrites(
            FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
        const auto initialTimesEntered = failPoint->initialTimesEntered();
        ASSERT_OK(indexer->insertAllDocumentsInCollection(opCtx, getNSS()));
        EXPECT_EQ(initialTimesEntered + 1,
                  (*failPoint)->waitForTimesEntered(initialTimesEntered + 1))
            << "Expected exactly one WCE during the duplicate-key recording retry";
    }

    // Count records directly in the constraint-violations ident: a read-based assertion would
    // still pass if a buggy retry inserted the duplicate twice.
    EXPECT_EQ(1u,
              countRecordsInIdent(opCtx, storageEngine, *indexBuildInfo.constraintViolationsIdent));

    indexer->abortIndexBuild(opCtx, coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, AbortWithoutCleanupAfterInsertingSingleDocument) {
    auto indexer = getIndexer();

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto specs = unittest::assertGet(indexer->init(operationContext(),
                                                   coll,
                                                   {},
                                                   MultiIndexBlock::kNoopOnInitFn,
                                                   MultiIndexBlock::InitMode::InitialSync,
                                                   boost::none));
    EXPECT_EQ(0U, specs.size());
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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
    EXPECT_EQ(1U, specs.size());

    indexer->setIsResumable(true);
    indexer->abortWithoutCleanup(operationContext(), coll.get());
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, coll->uuid());
    ASSERT_TRUE(indexBuildIdent);

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *indexBuildIdent});
}

TEST_F(MultiIndexBlockTest, PersistResumeStateOnRequestAndCommit) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
    EXPECT_EQ(1U, specs.size());

    indexer->setIsResumable(true);
    indexer->persistResumeState(operationContext(), coll.get());
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, coll->uuid());
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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
    EXPECT_EQ(1U, specs.size());

    indexer->setIsResumable(true);
    indexer->persistResumeState(operationContext(), coll.get());
    auto indexBuildIdent = findPersistedResumeState(
        operationContext(), buildUUID, IndexBuildPhaseEnum::kInitialized, coll->uuid());
    EXPECT_TRUE(indexBuildIdent);

    indexer->abortWithoutCleanup(operationContext(), coll.get());
    indexBuildIdent = findPersistedResumeState(operationContext(), buildUUID);
    ASSERT_TRUE(indexBuildIdent);

    validateTempTableIdentsOnTeardown({*indexBuildInfo1.sideWritesIdent, *indexBuildIdent});
}

TEST_F(MultiIndexBlockTest, InitWriteConflictException) {
    auto indexer = getIndexer();

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
        EXPECT_NE(status, ErrorCodes::IndexBuildAlreadyInProgress);
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
        EXPECT_EQ(secondaryIndexer
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
        EXPECT_EQ(secondaryIndexer
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
        EXPECT_EQ(secondaryIndexer
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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *storageEngine);

    {
        CollectionWriter coll(operationContext(), &acq);
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
        ASSERT_OK(Helpers::insert(
            operationContext(), acq.getCollectionPtr(), BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    {
        CollectionWriter coll(operationContext(), &acq);
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
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                              MODE_X);

        {
            CollectionWriter coll(operationContext(), &acq);
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
            ASSERT_OK(Helpers::insert(
                operationContext(), acq.getCollectionPtr(), BSON("_id" << 0 << "a" << 1)));
            wuow.commit();
        }

        ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    }

    {
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                              MODE_IX);
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
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                              MODE_X);
        CollectionWriter coll(operationContext(), &acq);
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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
    EXPECT_EQ(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
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
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    // The drop was registered at the commit timestamp. A timestamp <= commitTs should fail.
    ASSERT_THROWS_CODE(
        storageEngine->dropIdentTimestamped(operationContext(), sideWritesIdent, commitTs),
        DBException,
        ErrorCodes::ObjectIsBusy);

    // A timestamp greater than commitTs should succeed.
    storageEngine->dropIdentTimestamped(
        operationContext(), sideWritesIdent, Timestamp(commitTs.getSecs() + 1, 0));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

TEST_F(MultiIndexBlockTest, CommitWithNoCommitTimestampDropsImmediately) {
    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
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
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    // Without a commit timestamp, the drop is registered as Immediate.
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

TEST_F(MultiIndexBlockTest, AbortDropsTemporaryTables) {
    auto indexer = getIndexer();

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);

    // After abort, the ident is still in WiredTiger but is drop-pending in the reaper.
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

// With resumable primary-driven index builds enabled, MultiIndexBlock::init must create the
// per-build internal WT table whose ident matches ident::generateNewIndexBuildIdent(buildUUID),
// and MultiIndexBlock::commit must drop it.
TEST_F(MultiIndexBlockTest, CommitDropsResumablePrimaryDrivenIndexBuildTable) {
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard resumableEnabled{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                    true};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
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
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));
}

// When the build is not resumable, MultiIndexBlock::init must NOT eagerly create the per-build
// internal index build table — even if the container-write behavior is kReplicate.
TEST_F(MultiIndexBlockTest, InitSkipsResumablePrimaryDrivenIndexBuildTableWhenNotResumable) {
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    // Intentionally leave _isResumable at its default (false).

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// With resumable primary-driven index builds enabled, MultiIndexBlock::init must create the
// per-build internal WT table whose ident matches ident::generateNewIndexBuildIdent(buildUUID),
// and MultiIndexBlock::abort must drop it.
TEST_F(MultiIndexBlockTest, AbortDropsResumablePrimaryDrivenIndexBuildTable) {
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard resumableEnabled{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                    true};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);

    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), indexBuildIdent));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));
}

// With resumable PDIB enabled, the first call to drainBackgroundWrites must
// transition into kDrainWrites and persist a ResumeIndexInfo with that phase to the replicated
// internal-indexBuild-<UUID> table.
TEST_F(MultiIndexBlockTest, PdibPersistsResumeStateOnFirstDrain) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard resumableEnabled{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                    true};

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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

    // Before any drain, the table exists but no resume record has been written yet.
    {
        auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
        if (storageEngine->getEngine()->hasIdent(
                *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent)) {
            shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
            EXPECT_FALSE(index_builds::readAndParseResumeIndexInfo(
                storageEngine, operationContext(), indexBuildIdent));
        }
    }

    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(
        storageEngine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(IndexBuildPhaseEnum::kDrainWrites, resumeInfo->getPhase());
    EXPECT_EQ(coll->uuid(), resumeInfo->getCollectionUUID());

    // Re-entering drain (second/third pass in production) must not error and must leave the
    // record intact. Subsequent calls hit firstDrain == false and skip the persist.
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    ASSERT_OK(indexer->drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto laterResumeInfo = index_builds::readAndParseResumeIndexInfo(
        storageEngine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(laterResumeInfo);
    EXPECT_EQ(IndexBuildPhaseEnum::kDrainWrites, laterResumeInfo->getPhase());
    EXPECT_EQ(resumeInfo->getCollectionUUID(), laterResumeInfo->getCollectionUUID());

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, ResumePdibDuringDrain) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard resumableEnabled{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                    true};

    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 0 << "a" << 1)));
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
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 1 << "a" << 2)));
        wuow.commit();
    }

    // Count committed records in an internal table by iterating its cursor.
    auto countRecords = [&](std::string_view ident) -> size_t {
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
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(
        storageEngine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(IndexBuildPhaseEnum::kDrainWrites, resumeInfo->getPhase());

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

// Verifies that a resumed PDIB persists resume state at the firstDrain transition even when this
// generation's collection scan inserts no new records (e.g. resumed from kCollectionScan or
// kBulkLoad phase with no remaining records to process in this generation).
TEST_F(MultiIndexBlockTest, ResumedPdibPersistsStateOnFirstDrainWithNoNewRecords) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    // Force frequent sorter spills so the first build's scan persists kCollectionScan state.
    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    const auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    // Phase 1: original build runs the scan with low memory limit, triggering spills that
    // persist resume state at phase=kCollectionScan with a savedPosition mid-collection.
    {
        WriteUnitOfWork wuow(operationContext());
        std::string val(64 * 1024, 'a');
        for (auto i = 0; i < 20; ++i) {
            ASSERT_OK(
                Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
        }
        wuow.commit();
    }

    auto& indexer = *getIndexer();
    configurePdib(indexer);
    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto scanResumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(scanResumeInfo);
    // `insertAllDocumentsInCollection` ends in dumpInsertsFromBulk, which advances the persisted
    // phase to kBulkLoad. To exercise the "resume from mid-scan" code path below, synthesize a
    // kCollectionScan resume info from what was just persisted.
    ASSERT_EQ(IndexBuildPhaseEnum::kBulkLoad, scanResumeInfo->getPhase());
    scanResumeInfo->setPhase(IndexBuildPhaseEnum::kCollectionScan);

    // Tear down in-memory state; on-disk side-writes / sorter container / resume record persist.
    indexer.markAsCleanedUp();

    // Phase 2: resume from kCollectionScan. Pass a RecordId past every record in the collection
    // as the resumeAfter so the resumed scan inserts nothing — _lastRecordIdInserted remains
    // boost::none, and the firstDrain persist is therefore gated by _wasResumed.
    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    ASSERT_OK(resumedIndexer.init(operationContext(),
                                  coll,
                                  {indexBuildInfo},
                                  MultiIndexBlock::kNoopOnInitFn,
                                  MultiIndexBlock::InitMode::SteadyState,
                                  scanResumeInfo));

    // RecordIds for the 20 docs were assigned sequentially starting from 1, so RecordId(20)
    // is the last record. Scanning AFTER it finds nothing — _lastRecordIdInserted stays
    // boost::none, so the firstDrain persist is now gated solely on _wasResumed.
    ASSERT_OK(
        resumedIndexer.insertAllDocumentsInCollection(operationContext(), getNSS(), RecordId(20)));

    ASSERT_OK(
        resumedIndexer.drainBackgroundWrites(operationContext(),
                                             RecoveryUnit::ReadSource::kNoTimestamp,
                                             IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto drainResumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(drainResumeInfo);
    EXPECT_EQ(IndexBuildPhaseEnum::kDrainWrites, drainResumeInfo->getPhase());

    resumedIndexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// Empty-collection PDIB short-circuits insertAllDocumentsInCollection before the bulk loader is
// initialized. Drain entry must skip the persist (rather than null-deref in _constructStateObject)
// and not leave a resume record on disk.
TEST_F(MultiIndexBlockTest, PdibSkipsResumeStateOnEmptyCollection) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard resumableEnabled{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                    true};

    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
            EXPECT_FALSE(index_builds::readAndParseResumeIndexInfo(
                storageEngine, operationContext(), indexBuildIdent));
        }
    }

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// With the resumable PDIB feature flag disabled, drainBackgroundWrites does not persist
// resume info even on the PDIB path.
TEST_F(MultiIndexBlockTest, PdibDoesNotPersistResumeStateWhenFeatureFlagOff) {
    unittest::ServerParameterGuard pdibEnabled{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard resumableDisabled{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                                     false};

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    // When the resumable PDIB feature flag is off, production code uses kDoNotReplicate. We
    // mirror that here so init() takes the non-replicated path (no resume table created).
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kDoNotReplicate);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;

    indexer->setIsResumable(true);
    indexer->abortWithoutCleanup(operationContext(), coll.get());

    // After abortWithoutCleanup, the ident should still exist and not be drop-pending.
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

class MultiIndexBlockMetricsTest : public MultiIndexBlockTest,
                                   public ::testing::WithParamInterface<IndexBuildProtocol> {
protected:
    void setUp() override {
        MultiIndexBlockTest::setUp();
        switch (GetParam()) {
            case IndexBuildProtocol::kPrimaryDriven:
                _pdibEnabled.emplace("featureFlagPrimaryDrivenIndexBuilds", true);
                // Primary-driven index builds replicate their work through container writes, which
                // require this flag.
                _containerWritesEnabled.emplace("featureFlagContainerWrites", true);
                // Container writes refuse unless we're the primary; the fixture's mock returns
                // false from canAcceptWritesFor by default, so allow writes explicitly.
                static_cast<repl::ReplicationCoordinatorMock*>(
                    repl::ReplicationCoordinator::get(getServiceContext()))
                    ->alwaysAllowWrites(true);
                break;
            case IndexBuildProtocol::kSinglePhase:
            case IndexBuildProtocol::kTwoPhase:
                break;
        }
    }

    void configureIndexerForProtocol(MultiIndexBlock* indexer) {
        switch (GetParam()) {
            case IndexBuildProtocol::kPrimaryDriven:
                indexer->setBuildUUID(UUID::gen());
                indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
                indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
                break;
            case IndexBuildProtocol::kSinglePhase:
            case IndexBuildProtocol::kTwoPhase:
                break;
        }
    }

private:
    boost::optional<unittest::ServerParameterGuard> _pdibEnabled;
    boost::optional<unittest::ServerParameterGuard> _containerWritesEnabled;
};

std::string protocolTestName(IndexBuildProtocol protocol) {
    switch (protocol) {
        case IndexBuildProtocol::kSinglePhase:
            return "SinglePhase";
        case IndexBuildProtocol::kTwoPhase:
            return "TwoPhase";
        case IndexBuildProtocol::kPrimaryDriven:
            return "PrimaryDriven";
    }
    MONGO_UNREACHABLE;
}

TEST_P(MultiIndexBlockMetricsTest, BasicMetrics) {
    // Builds two indexes over the collection and checks the collection-scan metrics.
    auto indexer = getIndexer();
    configureIndexerForProtocol(indexer);

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

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 0 << "a" << 1)));
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 1 << "a" << 2)));
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

INSTANTIATE_TEST_SUITE_P(,
                         MultiIndexBlockMetricsTest,
                         ::testing::Values(IndexBuildProtocol::kSinglePhase,
                                           IndexBuildProtocol::kPrimaryDriven),
                         [](const ::testing::TestParamInfo<IndexBuildProtocol>& info) {
                             return protocolTestName(info.param);
                         });

TEST_F(MultiIndexBlockTest, AbortWithNoCommitTimestampDropsImmediately) {
    auto indexer = getIndexer();

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
    EXPECT_EQ(1U, specs.size());

    auto sideWritesIdent = *indexBuildInfo.sideWritesIdent;
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);

    // The ident is still in WiredTiger, pending drop in the reaper.
    EXPECT_TRUE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));

    // Without a commit timestamp, the drop is registered as Immediate.
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(), sideWritesIdent));
    EXPECT_FALSE(storageEngine->getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), sideWritesIdent));
}

// OpObserver that records onContainerInsert/onContainerUpdate calls so tests can verify that
// _writeStateToContainer went through the container_write path (which fires the observer).
class ResumeStateContainerInsertObserver : public OpObserverNoop {
public:
    void onContainerInsert(OperationContext*,
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) override {
        inserts.push_back({std::string{ident}, key, std::string{value.begin(), value.end()}});
    }

    void onContainerInsert(OperationContext*,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) override {
        inserts.push_back({std::string{ident},
                           std::string{key.begin(), key.end()},
                           std::string{value.begin(), value.end()}});
    }

    void onContainerUpdate(OperationContext*,
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) override {
        updates.push_back({std::string{ident}, key, std::string{value.begin(), value.end()}});
    }

    void onContainerUpdate(OperationContext*,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) override {
        updates.push_back({std::string{ident},
                           std::string{key.begin(), key.end()},
                           std::string{value.begin(), value.end()}});
    }

    struct Op {
        std::string ident;
        std::variant<int64_t, std::string> key;
        std::string value;
    };
    std::vector<Op> inserts;
    std::vector<Op> updates;

    size_t countInsertsForIdent(std::string_view ident) const {
        return std::count_if(
            inserts.begin(), inserts.end(), [&](const Op& op) { return op.ident == ident; });
    }

    size_t countUpdatesForIdent(std::string_view ident) const {
        return std::count_if(
            updates.begin(), updates.end(), [&](const Op& op) { return op.ident == ident; });
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

// Setup for a primary-driven (kReplicate) build: assigns a build UUID, flips the container-write
// behavior, inserts one document into the collection (so the scan doesn't short-circuit on
// emptiness), constructs a single-spec IndexBuildInfo, runs init and the collection scan so
// index.bulk is populated for _constructStateObject. Caller still owns the CollectionAcquisition /
// CollectionWriter and the feature-flag scope.
KReplicateBuildHandle setUpKReplicatePrimaryDrivenBuild(OperationContext* opCtx,
                                                        MultiIndexBlock* indexer,
                                                        CollectionAcquisition& acq,
                                                        CollectionWriter& coll,
                                                        const NamespaceString& nss) {
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.get(), BSON("_id" << 0 << "a" << 1)));
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

TEST_F(MultiIndexBlockTest, CommitToleratesKeysAlreadyInContainer) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    promoteMockReplCoordToPrimary(getServiceContext());
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    indexer->setBuildUUID(UUID::gen());
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    {
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

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
    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto* indexEntry = coll.get()->getIndexCatalog()->findIndexByName(
        operationContext(), "a_1", IndexCatalog::InclusionPolicy::kUnfinished);
    ASSERT(indexEntry);
    auto* iam = indexEntry->accessMethod()->asSortedData();
    ASSERT(iam);

    auto record = coll.get()->getCursor(operationContext())->next();
    ASSERT(record);
    auto recordId = record->id;

    auto& containerPool = PreallocatedContainerPool::get(operationContext());
    SharedBufferFragmentBuilder pooledBuilder{1024};
    KeyStringSet keys;
    KeyStringSet multikeyMetadataKeys;
    auto multikeyPaths = containerPool.multikeyPaths();
    iam->getKeys(operationContext(),
                 coll.get(),
                 indexEntry,
                 pooledBuilder,
                 BSON("_id" << 0 << "a" << 1),
                 InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                 SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                 &keys,
                 &multikeyMetadataKeys,
                 multikeyPaths.get(),
                 recordId);
    ASSERT_EQ(keys.size(), 1);

    {
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(container_write::insert(operationContext(),
                                          *shard_role_details::getRecoveryUnit(operationContext()),
                                          iam->getSortedDataInterface()->getContainer(),
                                          keys.begin()->getView(),
                                          keys.begin()->getTypeBitsView(),
                                          container_write::NonexistentKeyGuarantee{}));
        wuow.commit();
    }

    auto insertsBefore = observer.countInsertsForIdent(indexBuildInfo.indexIdent);
    EXPECT_GT(insertsBefore, 0);

    {
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(indexer->commit(operationContext(),
                                  coll.getWritableCollection(operationContext()),
                                  MultiIndexBlock::kNoopOnCreateEachFn,
                                  MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }

    // The commit should have skipped the key that was previously inserted.
    EXPECT_EQ(observer.countInsertsForIdent(indexBuildInfo.indexIdent), insertsBefore);
}

// When the build is hybrid (kDoNotReplicate), persistResumeState must not go through the
// container-write path even if the resumable PDIB feature flag is on.
TEST_F(MultiIndexBlockTest, HybridBuildDoesNotUseContainerWrites) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIsResumable(true);
    // No setContainerWriteBehavior — defaults to kDoNotReplicate (hybrid).

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
    EXPECT_EQ(0U, observer.inserts.size());

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, WriteStateToContainerOnSpillWhenResumable) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    // Lower the per-build memory limit to 1 MB.
    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    // Insert enough indexable data to trigger multiple spills against the 1 MB limit. 60 documents
    // with 64 KB strings (~3.84 MB total) guarantees at least 3 spills, exercising both the
    // initial insert and the subsequent update path in _writeStateToContainer().
    WriteUnitOfWork wuow(operationContext());
    std::string val(64 * 1024, 'a');
    for (auto i = 0; i < 60; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
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
    EXPECT_TRUE(engine.getEngine()->hasIdent(
        *shard_role_details::getRecoveryUnit(operationContext()), indexBuildIdent));

    // Insert the data into the sorter. The first spill will trigger one insert per record in the
    // index build table, and subsequent spills will trigger updates to the same keys.
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));
    EXPECT_EQ(observer.countInsertsForIdent(indexBuildIdent), 2);
    EXPECT_GE(observer.countUpdatesForIdent(indexBuildIdent), 2);

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(resumeInfo.has_value());
    EXPECT_EQ(resumeInfo->getBuildUUID(), buildUUID);
    EXPECT_EQ(resumeInfo->getCollectionUUID(), coll->uuid());
    ASSERT_EQ(resumeInfo->getIndexes().size(), 1);
    EXPECT_EQ(resumeInfo->getIndexes()[0].getSpec()["name"].String(), "a_1");

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, OnSpillCallbackSeesLatestRecordIdAndKeyCount) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    {
        WriteUnitOfWork wuow{operationContext()};
        std::string val(2 * 1024 * 1024, 'a');
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << 0 << "a" << val)));
        wuow.commit();
    }

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

    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(
        &engine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kBulkLoad);

    ASSERT_EQ(resumeInfo->getIndexes().size(), 1);
    ASSERT_TRUE(resumeInfo->getIndexes()[0].getNumKeys());
    EXPECT_EQ(*resumeInfo->getIndexes()[0].getNumKeys(), 1);

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, OnSpillRecordsLastSpilledRecordId) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    WriteUnitOfWork wuow{operationContext()};
    std::string val(64 * 1024, 'a');
    auto numDocs = 20;
    for (auto i = 0; i < numDocs; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
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

    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(
        &engine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kBulkLoad);

    ASSERT_EQ(resumeInfo->getIndexes().size(), 1);
    auto lastSpilledRecordId = resumeInfo->getIndexes()[0].getLastSpilledRecordId();
    ASSERT_TRUE(lastSpilledRecordId);
    // The most recent spill captures whatever _lastRecordIdInserted held at that moment, bounded
    // by the collection's RecordId range.
    EXPECT_GT(lastSpilledRecordId->getLong(), 0);
    EXPECT_LE(lastSpilledRecordId->getLong(), numDocs);

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, OnSpillRecordsLastSpilledRecordIdForMultipleIndexes) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    WriteUnitOfWork wuow{operationContext()};
    std::string val(64 * 1024, 'a');
    auto numDocs = 20;
    for (auto i = 0; i < numDocs; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(),
                                  coll.get(),
                                  BSON("_id" << i << "a" << val << "b" << val << "c" << val)));
    }
    wuow.commit();

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto makeInfo = [&](std::string_view field, std::string_view ident) {
        return IndexBuildInfo(BSON("key" << BSON(field << 1) << "name"
                                         << (std::string(field) + "_1") << "v"
                                         << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                              std::string(ident),
                              engine);
    };
    std::vector<IndexBuildInfo> infos{
        makeInfo("a", "index-1"), makeInfo("b", "index-2"), makeInfo("c", "index-3")};

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           infos,
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(
        &engine, operationContext(), ident::generateNewIndexBuildIdent(buildUUID));
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kBulkLoad);

    // Each index's onSpill callback writes the same `_lastRecordIdInserted` into its
    // `lastSpilledRecordId`. Verify every index recorded a value and that they all agree
    // (using the first index's value as the shared reference).
    EXPECT_EQ(resumeInfo->getIndexes().size(), infos.size());
    boost::optional<RecordId> firstLastSpilled;
    for (auto&& indexInfo : resumeInfo->getIndexes()) {
        auto lastSpilledRecordId = indexInfo.getLastSpilledRecordId();
        ASSERT_TRUE(lastSpilledRecordId);
        if (!firstLastSpilled) {
            firstLastSpilled = lastSpilledRecordId;
        }
        EXPECT_EQ(lastSpilledRecordId->getLong(), firstLastSpilled->getLong());
        EXPECT_GT(lastSpilledRecordId->getLong(), 0);
        EXPECT_LE(lastSpilledRecordId->getLong(), numDocs);
    }

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// Sorter spills during the collection scan persist resume state with
// phase=kCollectionScan and the current scan position. On step-up, a new MultiIndexBlock
// initialized from that state must reopen the existing tables and resume the scan from the saved
// position rather than starting over.
TEST_F(MultiIndexBlockTest, ResumePdibDuringCollectionScan) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    const auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    auto& indexer = *getIndexer();
    configurePdib(indexer);
    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    // 20 documents of 64 KB exceed the 1 MB sorter limit and force multiple mid-scan spills.
    {
        WriteUnitOfWork wuow(operationContext());
        std::string val(64 * 1024, 'a');
        for (auto i = 0; i < 20; ++i) {
            ASSERT_OK(
                Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
        }
        wuow.commit();
    }

    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(buildUUID, resumeInfo->getBuildUUID());
    // Override the phase to kCollectionScan to exercise the resume-from-mid-scan code path below.
    EXPECT_EQ(IndexBuildPhaseEnum::kBulkLoad, resumeInfo->getPhase());
    resumeInfo->setPhase(IndexBuildPhaseEnum::kCollectionScan);
    EXPECT_EQ(coll->uuid(), resumeInfo->getCollectionUUID());

    // The persisted scan position is a valid RecordId inside the 20-doc collection.
    // A valid spill checkpoint must satisfy 0 < position <= 20.
    // The resume point is the minimum `lastSpilledRecordId` across all indexes.
    auto minLastSpilled = index_builds::minLastSpilledRecordId(resumeInfo->getIndexes());
    ASSERT_TRUE(minLastSpilled);
    const auto savedPosition = *minLastSpilled;
    ASSERT_TRUE(savedPosition.isValid());
    EXPECT_GT(savedPosition.getLong(), 0);
    EXPECT_LE(savedPosition.getLong(), 20);

    // Per-index state: every ident allocated by the original IndexBuildInfo round-trips through
    // the persisted ResumeIndexInfo so that the resumed indexer reopens the same tables.
    ASSERT_EQ(1U, resumeInfo->getIndexes().size());
    const auto& indexState = resumeInfo->getIndexes()[0];
    EXPECT_EQ(*indexBuildInfo.sideWritesIdent, indexState.getSideWritesTable());
    ASSERT_TRUE(indexState.getSkippedRecordTrackerTable());
    EXPECT_EQ(*indexBuildInfo.skippedRecordsIdent, *indexState.getSkippedRecordTrackerTable());
    ASSERT_TRUE(indexState.getStorageIdentifier());
    EXPECT_EQ(*indexBuildInfo.sorterIdent, *indexState.getStorageIdentifier());
    EXPECT_EQ("a_1", indexState.getSpec()["name"].String());
    EXPECT_FALSE(indexState.getIsMultikey());

    // Simulate step-down: drop in-memory state without touching on-disk tables.
    indexer.markAsCleanedUp();

    // Simulate step-up resume: a new MultiIndexBlock for the same buildUUID initialized from the
    // persisted kCollectionScan resume info.
    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    ASSERT_OK(resumedIndexer.init(operationContext(),
                                  coll,
                                  {indexBuildInfo},
                                  MultiIndexBlock::kNoopOnInitFn,
                                  MultiIndexBlock::InitMode::SteadyState,
                                  resumeInfo));

    // Continue the collection scan from the saved position. If savedPosition < 20 there are
    // remaining records to scan; if savedPosition == 20 the cursor is past the end and the call
    // is a no-op. Both must succeed.
    ASSERT_OK(
        resumedIndexer.insertAllDocumentsInCollection(operationContext(), getNSS(), savedPosition));

    resumedIndexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

// A build that spilled during its scan and then resumed from the scan phase must still be treated
// as "spilled" even when the resumed scan portion is small enough that it does not spill again.
TEST_F(MultiIndexBlockTest, ResumeFromScanWithSpilledRangesPersistsLoadPhaseWithoutRespilling) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    const auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    // Original build: 20 docs of 64 KB exceed the 1 MB limit and spill during the scan.
    auto& indexer = *getIndexer();
    configurePdib(indexer);
    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));
    {
        WriteUnitOfWork wuow{operationContext()};
        std::string val(64 * 1024, 'a');
        for (auto i = 0; i < 20; ++i) {
            ASSERT_OK(
                Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
        }
        wuow.commit();
    }
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(resumeInfo);
    // Precondition: the original build spilled, so its persisted state carries ranges.
    ASSERT_TRUE(resumeInfo->getIndexes()[0].getRanges());
    EXPECT_FALSE(resumeInfo->getIndexes()[0].getRanges()->empty());
    // Simulate a step-down mid-scan: override the persisted phase to kCollectionScan.
    resumeInfo->setPhase(IndexBuildPhaseEnum::kCollectionScan);
    auto savedPosition = index_builds::minLastSpilledRecordId(resumeInfo->getIndexes());
    EXPECT_TRUE(savedPosition);

    indexer.markAsCleanedUp();

    // Raise the memory budget so the resumed scan does not spill again, and install a fresh
    // observer so it captures only the resumed build's container writes.
    maxIndexBuildMemoryUsageMegabytes.store(1024);
    auto& observer = installResumeStateContainerObserver(operationContext());

    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    ASSERT_OK(resumedIndexer.init(operationContext(),
                                  coll,
                                  {indexBuildInfo},
                                  MultiIndexBlock::kNoopOnInitFn,
                                  MultiIndexBlock::InitMode::SteadyState,
                                  resumeInfo));
    ASSERT_OK(resumedIndexer.insertAllDocumentsInCollection(
        operationContext(), getNSS(), *savedPosition));

    // The resumed build saw the prior ranges and treated it as spilled so, even though the resumed
    // scan did not spill, the load transition force-spilled the remainder and updated the persisted
    // phase to the load phase.
    auto isMetadataKey = [](const auto& op) {
        return std::holds_alternative<int64_t>(op.key) && std::get<int64_t>(op.key) == 1;
    };
    bool wroteBulkLoadMetadata = false;
    auto scanForBulkLoad = [&](const std::vector<ResumeStateContainerInsertObserver::Op>& ops) {
        for (const auto& op : ops) {
            if (op.ident != indexBuildIdent || !isMetadataKey(op)) {
                continue;
            }
            auto metadata = IndexBuildMetadata::parse(BSONObj(op.value.data()),
                                                      IDLParserContext("IndexBuildMetadata"));
            if (metadata.getPhase() == IndexBuildPhaseEnum::kBulkLoad) {
                wroteBulkLoadMetadata = true;
            }
        }
    };
    scanForBulkLoad(observer.inserts);
    scanForBulkLoad(observer.updates);
    EXPECT_TRUE(wroteBulkLoadMetadata);

    resumedIndexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, PdibResumedScanSkipsRecordsAtOrBeforePerIndexLastSpilledRecordId) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto makeInfo = [&](std::string_view field, std::string_view ident) {
        return IndexBuildInfo{BSON("key" << BSON(field << 1) << "name"
                                         << (std::string(field) + "_1") << "v"
                                         << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                              std::string(ident),
                              engine};
    };
    std::vector<IndexBuildInfo> infos{
        makeInfo("a", "index-1"), makeInfo("b", "index-2"), makeInfo("c", "index-3")};

    // The setup runs phase 1 with `initialDocs` docs, capturing a real ResumeIndexInfo whose
    // originalLastSpilled falls in (0, initialDocs]. Then `extraDocs` more docs are inserted before
    // resume, so the resumed scan has records originalLastSpilled+1..totalDocs available to
    // differentiate per-index skip behavior. Choosing offsets 0, 3, 6 ensures
    // originalLastSpilled + 6 < totalDocs for any originalLastSpilled in (0, initialDocs] = (0,
    // 20].
    constexpr auto initialDocs = 20;
    constexpr auto extraDocs = 10;
    constexpr auto totalDocs = initialDocs + extraDocs;

    boost::optional<ResumeIndexInfo> resumeInfo;
    {
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                              MODE_X);
        CollectionWriter coll{operationContext(), &acq};

        {
            WriteUnitOfWork wuow{operationContext()};
            std::string val(64 * 1024, 'a');
            for (auto i = 0; i < initialDocs; ++i) {
                ASSERT_OK(
                    Helpers::insert(operationContext(),
                                    coll.get(),
                                    BSON("_id" << i << "a" << val << "b" << val << "c" << val)));
            }
            wuow.commit();
        }

        auto& indexer = *getIndexer();
        configurePdib(indexer);
        ASSERT_OK(indexer.init(operationContext(),
                               coll,
                               infos,
                               MultiIndexBlock::kNoopOnInitFn,
                               MultiIndexBlock::InitMode::SteadyState,
                               boost::none));

        // Run phase 1 to completion. The three indexes share a small per-index memory budget, so
        // the sorters spill repeatedly during the scan; the last spill captures
        // lastSpilledRecordId = originalLastSpilled for some value in (0, initialDocs].
        ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

        auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
        resumeInfo =
            index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
        ASSERT_TRUE(resumeInfo);
        ASSERT_EQ(resumeInfo->getIndexes().size(), infos.size());

        // To exercise the resume-from-mid-scan code path below, synthesize a kCollectionScan resume
        // info from what was just persisted.
        ASSERT_EQ(IndexBuildPhaseEnum::kBulkLoad, resumeInfo->getPhase());
        resumeInfo->setPhase(IndexBuildPhaseEnum::kCollectionScan);

        indexer.markAsCleanedUp();
    }

    // Insert `extraDocs` more documents so the resumed scan sees records beyond
    // originalLastSpilled. These docs exist on disk but aren't represented in any sorter's spilled
    // state.
    {
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                              MODE_X);
        WriteUnitOfWork wuow{operationContext()};
        std::string val(64 * 1024, 'a');
        for (auto i = initialDocs; i < totalDocs; ++i) {
            ASSERT_OK(Helpers::insert(operationContext(),
                                      acq.getCollectionPtr(),
                                      BSON("_id" << i << "a" << val << "b" << val << "c" << val)));
        }
        wuow.commit();
    }

    auto persistedIndexes = resumeInfo->getIndexes();
    ASSERT_TRUE(persistedIndexes[0].getLastSpilledRecordId());
    auto originalLastSpilled = persistedIndexes[0].getLastSpilledRecordId()->getLong();

    // The expected per-index numEntries below is originalLastSpilled pre-existing spilled keys plus
    // `totalDocs - mutated` keys added by the resumed scan, which simplifies to
    // `totalDocs - offset` independent of originalLastSpilled. Offsets 0, 3, 6 keep the test stable
    // for any originalLastSpilled in (0, initialDocs].
    ASSERT_GTE(originalLastSpilled, 1);
    ASSERT_LE(originalLastSpilled, initialDocs);

    persistedIndexes[0].setLastSpilledRecordId(RecordId{originalLastSpilled});
    persistedIndexes[1].setLastSpilledRecordId(RecordId{originalLastSpilled + 3});
    persistedIndexes[2].setLastSpilledRecordId(RecordId{originalLastSpilled + 6});
    resumeInfo->setIndexes(std::move(persistedIndexes));

    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(getNSS(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collWriter{operationContext(), &coll};
        ASSERT_OK(resumedIndexer.init(operationContext(),
                                      collWriter,
                                      infos,
                                      MultiIndexBlock::kNoopOnInitFn,
                                      MultiIndexBlock::InitMode::SteadyState,
                                      resumeInfo));
    }

    // Resume the collection scan from the beginning.
    ASSERT_OK(resumedIndexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    // Per-index numEntries = originalLastSpilled (pre-existing spilled keys) + (totalDocs -
    // mutated) keys added by the resumed scan. With mutated_i = originalLastSpilled + offset_i,
    // this collapses to totalDocs - offset_i, independent of originalLastSpilled.
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(getNSS(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        auto numEntries = [&](std::string_view name) {
            auto* entry = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
                operationContext(), name, IndexCatalog::InclusionPolicy::kUnfinished);
            ASSERT(entry);
            return entry->accessMethod()->asSortedData()->getSortedDataInterface()->numEntries(
                operationContext(), *shard_role_details::getRecoveryUnit(operationContext()));
        };
        EXPECT_EQ(numEntries("a_1"), totalDocs);
        EXPECT_EQ(numEntries("b_1"), totalDocs - 3);
        EXPECT_EQ(numEntries("c_1"), totalDocs - 6);
    }

    {
        auto collForAbort = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(getNSS(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collWriter{operationContext(), &collForAbort};
        resumedIndexer.abortIndexBuild(
            operationContext(), collWriter, MultiIndexBlock::kNoopOnCleanUpFn);
    }
}

TEST_F(MultiIndexBlockTest, ResumeRestoresLastSpilledRecordId) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    auto& indexer = *getIndexer();
    configurePdib(indexer);
    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    {
        WriteUnitOfWork wuow{operationContext()};
        std::string val(64 * 1024, 'a');
        for (auto i = 0; i < 20; ++i) {
            ASSERT_OK(
                Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
        }
        wuow.commit();
    }

    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto originalResumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(originalResumeInfo);
    // Override the phase to kCollectionScan to exercise the resume-from-mid-scan code path below.
    EXPECT_EQ(IndexBuildPhaseEnum::kBulkLoad, originalResumeInfo->getPhase());
    originalResumeInfo->setPhase(IndexBuildPhaseEnum::kCollectionScan);
    ASSERT_EQ(originalResumeInfo->getIndexes().size(), 1);
    auto originalLastSpilledRecordId = originalResumeInfo->getIndexes()[0].getLastSpilledRecordId();
    ASSERT_TRUE(originalLastSpilledRecordId);

    // Simulate step-down.
    indexer.markAsCleanedUp();

    // Simulate step-up.
    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    ASSERT_OK(resumedIndexer.init(operationContext(),
                                  coll,
                                  {indexBuildInfo},
                                  MultiIndexBlock::kNoopOnInitFn,
                                  MultiIndexBlock::InitMode::SteadyState,
                                  originalResumeInfo));
    resumedIndexer.persistResumeState(operationContext(), coll.get());

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumedResumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT_TRUE(resumedResumeInfo);
    ASSERT_EQ(resumedResumeInfo->getIndexes().size(), 1);
    auto resumedLastSpilled = resumedResumeInfo->getIndexes()[0].getLastSpilledRecordId();
    ASSERT_TRUE(resumedLastSpilled);
    EXPECT_EQ(resumedLastSpilled->getLong(), originalLastSpilledRecordId->getLong());

    resumedIndexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, ResumePdibDuringLoad) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    unittest::ServerParameterGuard resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 5};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());

    auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       engine);

    boost::optional<ResumeIndexInfo> resumeInfo;
    UUID collectionUUID = UUID::gen();
    auto numDocs = 20;
    {
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                              MODE_X);
        CollectionWriter coll(operationContext(), &acq);
        collectionUUID = acq.uuid();

        auto& indexer = *getIndexer();
        configurePdib(indexer);
        ASSERT_OK(indexer.init(operationContext(),
                               coll,
                               {indexBuildInfo},
                               MultiIndexBlock::kNoopOnInitFn,
                               MultiIndexBlock::InitMode::SteadyState,
                               boost::none));

        {
            WriteUnitOfWork wuow(operationContext());
            std::string val(64 * 1024, 'a');
            for (auto i = 0; i < numDocs; ++i) {
                ASSERT_OK(Helpers::insert(
                    operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
            }
            wuow.commit();
        }

        ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

        auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
        resumeInfo =
            index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
        ASSERT_TRUE(resumeInfo);
        EXPECT_EQ(buildUUID, resumeInfo->getBuildUUID());
        EXPECT_EQ(IndexBuildPhaseEnum::kBulkLoad, resumeInfo->getPhase());
        EXPECT_EQ(collectionUUID, resumeInfo->getCollectionUUID());

        EXPECT_FALSE(resumeInfo->getCollectionScanPosition());
        ASSERT_EQ(resumeInfo->getIndexes().size(), 1);
        const auto& indexState = resumeInfo->getIndexes()[0];
        EXPECT_EQ(*indexBuildInfo.sideWritesIdent, indexState.getSideWritesTable());
        ASSERT_TRUE(indexState.getSkippedRecordTrackerTable());
        EXPECT_EQ(*indexState.getSkippedRecordTrackerTable(), *indexBuildInfo.skippedRecordsIdent);
        ASSERT_TRUE(indexState.getStorageIdentifier());
        EXPECT_EQ(*indexState.getStorageIdentifier(), *indexBuildInfo.sorterIdent);
        EXPECT_EQ(indexState.getSpec()["name"].String(), "a_1");
        EXPECT_FALSE(indexState.getIsMultikey());
        ASSERT_TRUE(indexState.getRanges());
        EXPECT_FALSE(indexState.getRanges()->empty());

        indexer.markAsCleanedUp();
    }

    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(getNSS(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collWriter(operationContext(), &coll);
        ASSERT_OK(resumedIndexer.init(operationContext(),
                                      collWriter,
                                      {indexBuildInfo},
                                      MultiIndexBlock::kNoopOnInitFn,
                                      MultiIndexBlock::InitMode::SteadyState,
                                      resumeInfo));
    }

    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(getNSS(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
        ASSERT_OK(resumedIndexer.dumpInsertsFromBulk(operationContext(), coll));

        auto* entry = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
            operationContext(), "a_1", IndexCatalog::InclusionPolicy::kUnfinished);
        ASSERT(entry);
        EXPECT_EQ(entry->accessMethod()->asSortedData()->getSortedDataInterface()->numEntries(
                      operationContext(), *shard_role_details::getRecoveryUnit(operationContext())),
                  numDocs);
    }

    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(getNSS(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collWriter(operationContext(), &coll);
        resumedIndexer.abortIndexBuild(
            operationContext(), collWriter, MultiIndexBlock::kNoopOnCleanUpFn);
    }
}

TEST_F(MultiIndexBlockTest, ResumedPdibSpillerContinuesContainerKeysPastPriorRanges) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    unittest::ServerParameterGuard memUsage{"maxIndexBuildMemoryUsageMegabytes", 1};

    promoteMockReplCoordToPrimary(getServiceContext());

    auto buildUUID = UUID::gen();
    auto configurePdib = [&](MultiIndexBlock& idx) {
        idx.setBuildUUID(buildUUID);
        idx.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
        idx.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
        idx.setIsResumable(true);
    };

    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};
    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();

    IndexBuildInfo indexBuildInfo{BSON("key" << BSON("a" << 1) << "name"
                                             << "a_1"
                                             << "v"
                                             << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                                  "index-1",
                                  engine};

    auto insertBigDocs = [&](int firstId, int count) {
        WriteUnitOfWork wuow{operationContext()};
        std::string val(64 * 1024, 'a');
        for (int i = 0; i < count; ++i) {
            ASSERT_OK(Helpers::insert(
                operationContext(), coll.get(), BSON("_id" << (firstId + i) << "a" << val)));
        }
        wuow.commit();
    };

    auto& indexer = *getIndexer();
    configurePdib(indexer);
    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    // Insert enough data for the indexer to spill.
    insertBigDocs(0, 60);
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT(resumeInfo);
    ASSERT_EQ(resumeInfo->getIndexes().size(), 1);
    auto& priorRanges = resumeInfo->getIndexes()[0].getRanges();
    ASSERT(priorRanges);
    ASSERT_FALSE(priorRanges->empty());
    auto prevLastEnd = priorRanges->back().getEnd();
    EXPECT_GT(prevLastEnd, 1);

    indexer.markAsCleanedUp();
    resumeInfo->setPhase(IndexBuildPhaseEnum::kCollectionScan);

    // Insert more documents so that the resumed indexer does more spilling.
    insertBigDocs(60, 60);

    MultiIndexBlock resumedIndexer;
    configurePdib(resumedIndexer);
    ASSERT_OK(resumedIndexer.init(operationContext(),
                                  coll,
                                  {indexBuildInfo},
                                  MultiIndexBlock::kNoopOnInitFn,
                                  MultiIndexBlock::InitMode::SteadyState,
                                  resumeInfo));

    // Re-scan the whole collection so the resumed indexer spills again.
    ASSERT_OK(
        resumedIndexer.insertAllDocumentsInCollection(operationContext(), getNSS(), boost::none));

    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo2 =
        index_builds::readAndParseResumeIndexInfo(&engine, operationContext(), indexBuildIdent);
    ASSERT(resumeInfo2);
    ASSERT_EQ(resumeInfo2->getIndexes().size(), 1);
    const auto& allRanges = resumeInfo2->getIndexes()[0].getRanges();
    ASSERT_TRUE(allRanges && !allRanges->empty());

    for (size_t i = 0; i < allRanges->size(); ++i) {
        EXPECT_GE((*allRanges)[i].getStart(), prevLastEnd)
            << "range " << i << " starts at " << (*allRanges)[i].getStart()
            << ", below prevLastEnd=" << prevLastEnd
            << " — resumed spiller did not advance past prior ranges";
    }
    for (size_t i = 1; i < allRanges->size(); ++i) {
        EXPECT_GE((*allRanges)[i].getStart(), (*allRanges)[i - 1].getEnd());
    }

    resumedIndexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, DoNotWriteStateToContainerOnSpillWhenNotResumable) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    // Lower the per-build memory limit to 1 MB.
    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

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
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
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

TEST_F(MultiIndexBlockTest, SeedWriteSurvivesWriteConflict) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    promoteMockReplCoordToPrimary(getServiceContext());

    auto indexer = getIndexer();
    const auto buildUUID = UUID::gen();
    indexer->setBuildUUID(buildUUID);
    indexer->setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer->setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer->setIsResumable(true);

    AutoGetCollection autoColl(operationContext(), getNSS(), MODE_X);
    CollectionWriter coll(operationContext(), autoColl);
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), *autoColl, BSON("_id" << 0 << "a" << 1)));
        wuow.commit();
    }

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

    // Inject a single WCE — the seed write's `writeConflictRetry` must absorb it and retry.
    auto failPoint = enableWriteConflictForWrites(
        FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
    const auto initialTimesEntered = failPoint->initialTimesEntered();
    ASSERT_OK(indexer->insertAllDocumentsInCollection(operationContext(), getNSS()));
    EXPECT_EQ(initialTimesEntered + 1, (*failPoint)->waitForTimesEntered(initialTimesEntered + 1));

    // After the retry, the table contains exactly 1 + numIndexes records.
    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    auto rs =
        storageEngine->getEngine()->getInternalRecordStore(ru, indexBuildIdent, KeyFormat::Long);
    auto cursor = rs->getCursor(operationContext(), ru);
    size_t count = 0;
    while (cursor->next()) {
        ++count;
    }
    EXPECT_EQ(count, 2);

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, SpillsDoNotWriteIndexBuildMetadata) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    unittest::ServerParameterGuard memUsage{"maxIndexBuildMemoryUsageMegabytes", 2};
    unittest::ServerParameterGuard iteratorsMemoryPct{"maxIteratorsMemoryUsagePercentage", 1};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    {
        WriteUnitOfWork wuow{operationContext()};
        std::string val(64 * 1024, 'a');
        for (auto i = 0; i < 300; ++i) {
            ASSERT_OK(
                Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
        }
        wuow.commit();
    }

    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *operationContext()->getServiceContext()->getStorageEngine());

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    std::vector<IndexBuildMetadata> metadataWrites;
    auto collectMetadata = [&](const std::vector<ResumeStateContainerInsertObserver::Op>& ops) {
        for (const auto& op : ops) {
            if (op.ident != indexBuildIdent) {
                continue;
            }
            const auto* keyPtr = std::get_if<int64_t>(&op.key);
            if (!keyPtr || *keyPtr != 1) {
                continue;
            }
            metadataWrites.push_back(IndexBuildMetadata::parse(
                BSONObj(op.value.data()), IDLParserContext("IndexBuildMetadata")));
        }
    };
    collectMetadata(observer.inserts);
    collectMetadata(observer.updates);

    // The only key=1 writes are the transitions to the scan and load phases.
    ASSERT_EQ(metadataWrites.size(), 2);
    EXPECT_EQ(buildUUID, metadataWrites[0].getBuildUUID());
    EXPECT_EQ(buildUUID, metadataWrites[1].getBuildUUID());
    EXPECT_EQ(IndexBuildPhaseEnum::kCollectionScan, metadataWrites[0].getPhase());
    EXPECT_EQ(IndexBuildPhaseEnum::kBulkLoad, metadataWrites[1].getPhase());

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}


TEST_F(MultiIndexBlockTest, LoadWritesResumeStatePeriodicallyForPrimaryDrivenBuild) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    // Force frequent batch commits and resume-state writes during the load phase.
    unittest::ServerParameterGuard insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    unittest::ServerParameterGuard resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 10};
    // Periodic load-phase resume-state writes only happen when the build spilled during the scan.
    // Force spilling with a tiny memory budget and large index keys.
    unittest::ServerParameterGuard memUsage{"maxIndexBuildMemoryUsageMegabytes", 2};
    unittest::ServerParameterGuard iteratorsMemoryPct{"maxIteratorsMemoryUsagePercentage", 1};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    // 50 distinct ~64KB keys (~3.2MB total) exceed the 2MB budget above, forcing at least one spill
    // during the scan phase.
    WriteUnitOfWork wuow(operationContext());
    for (auto i = 0; i < 50; ++i) {
        ASSERT_OK(Helpers::insert(
            operationContext(),
            coll.get(),
            BSON("_id" << i << "a" << (std::to_string(i) + std::string(64 * 1024, 'a')))));
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

    // Collect every per-index resume-state record (key=2) written across the build and parse each
    // IndexStateInfo. Records written during the scan phase carry a `lastSpilledRecordId`, while
    // the initial seed (written before any spill) and the periodic load phase writes do not.
    auto isPerIndexKey = [](const auto& op) {
        return std::holds_alternative<int64_t>(op.key) && std::get<int64_t>(op.key) == 2;
    };
    size_t scanSpillWrites = 0;    // onSpill writes during the scan (carry lastSpilledRecordId)
    size_t seedAndLoadWrites = 0;  // seed + periodic load-phase writes (no lastSpilledRecordId)
    auto classify = [&](const std::vector<ResumeStateContainerInsertObserver::Op>& ops) {
        for (const auto& op : ops) {
            if (op.ident != indexBuildIdent || !isPerIndexKey(op)) {
                continue;
            }
            auto info =
                IndexStateInfo::parse(BSONObj(op.value.data()), IDLParserContext("IndexStateInfo"));
            if (info.getLastSpilledRecordId()) {
                ++scanSpillWrites;
            } else {
                ++seedAndLoadWrites;
            }
        }
    };
    classify(observer.inserts);
    classify(observer.updates);

    // The build spilled during the scan, so at least one onSpill write carries a
    // lastSpilledRecordId.
    EXPECT_GE(scanSpillWrites, 1);
    // 1 seed + 5 periodic load-phase writes (50 keys / interval 10), none of which carry a
    // lastSpilledRecordId.
    EXPECT_EQ(seedAndLoadWrites, 6);

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, NonSpillingPrimaryDrivenBuildDoesNotPersistLoadPhase) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};

    promoteMockReplCoordToPrimary(getServiceContext());

    auto indexer = getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    // Builds an index over a single small document, which is not large enough to spill.
    auto handle =
        setUpKReplicatePrimaryDrivenBuild(operationContext(), indexer, acq, coll, getNSS());

    // Because the build never spilled, the persisted resume state must still be at the
    // scan phase (so a resume rescans the collection), not the load phase.
    auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
    shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(
        &engine, operationContext(), handle.indexBuildIdent);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kCollectionScan);

    indexer->abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, NonSpillingPrimaryDrivenBuildSkipsPeriodicLoadResumeStateWrites) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    // A tiny resume-state interval would trigger periodic load-phase writes if the build spilled.
    unittest::ServerParameterGuard insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    unittest::ServerParameterGuard resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 10};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    // Small documents under the default memory budget never spill the sorter.
    WriteUnitOfWork wuow{operationContext()};
    for (auto i = 0; i < 50; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << i)));
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

    // The build never spilled, so the only container writes are the initial seed.
    EXPECT_EQ(observer.countInsertsForIdent(indexBuildIdent), 2);
    EXPECT_EQ(observer.countUpdatesForIdent(indexBuildIdent), 0);

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}

TEST_F(MultiIndexBlockTest, LoadDoesNotPeriodicallyWriteWhenNotResumable) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    unittest::ServerParameterGuard insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    unittest::ServerParameterGuard resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 10};

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll(operationContext(), &acq);

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(false);

    WriteUnitOfWork wuow(operationContext());
    for (auto i = 0; i < 50; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << i)));
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

TEST_F(MultiIndexBlockTest, LastSpilledRecordIdIsNotPersistedDuringLoadPhase) {
    unittest::ServerParameterGuard ffContainerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard ffPDIB{"featureFlagPrimaryDrivenIndexBuilds", true};
    unittest::ServerParameterGuard ffResumable{"featureFlagResumablePrimaryDrivenIndexBuilds",
                                               true};
    unittest::ServerParameterGuard insertionBatchSize{
        "primaryDrivenIndexBuildIndexInsertionBatchSize", 5};
    unittest::ServerParameterGuard resumeStateInterval{
        "primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys", 10};

    auto prevMemLimitMB = maxIndexBuildMemoryUsageMegabytes.swap(1);
    ON_BLOCK_EXIT([prevMemLimitMB] { maxIndexBuildMemoryUsageMegabytes.store(prevMemLimitMB); });

    promoteMockReplCoordToPrimary(getServiceContext());
    auto& observer = installResumeStateContainerObserver(operationContext());

    auto& indexer = *getIndexer();
    auto acq =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), getNSS(), AcquisitionPrerequisites::kWrite),
                          MODE_X);
    CollectionWriter coll{operationContext(), &acq};

    auto buildUUID = UUID::gen();
    indexer.setBuildUUID(buildUUID);
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kPrimaryDriven);
    indexer.setContainerWriteBehavior(ContainerWriteBehavior::kReplicate);
    indexer.setIsResumable(true);

    WriteUnitOfWork wuow{operationContext()};
    std::string val(32 * 1024, 'a');
    for (auto i = 0; i < 50; ++i) {
        ASSERT_OK(Helpers::insert(operationContext(), coll.get(), BSON("_id" << i << "a" << val)));
    }
    wuow.commit();

    auto indexBuildInfo =
        IndexBuildInfo(BSON("key" << BSON("a" << 1) << "name"
                                  << "a_1"
                                  << "v" << static_cast<int>(IndexConfig::kLatestIndexVersion)),
                       "index-1",
                       *operationContext()->getServiceContext()->getStorageEngine());

    ASSERT_OK(indexer.init(operationContext(),
                           coll,
                           {indexBuildInfo},
                           MultiIndexBlock::kNoopOnInitFn,
                           MultiIndexBlock::InitMode::SteadyState,
                           boost::none));

    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_OK(indexer.insertAllDocumentsInCollection(operationContext(), getNSS()));

    // The IndexBuildMetadata lives at key=1 and the per-index IndexStateInfo lives at key=2.
    // They are written within a single WUOW, so they appear adjacent in the observer's per-vector
    // capture. Pair them up to recover the (phase, IndexStateInfo) tuples this test is checking.
    std::vector<IndexStateInfo> scanStates;
    std::vector<IndexStateInfo> loadStates;
    boost::optional<IndexBuildPhaseEnum> currentPhase;
    auto pairWrites = [&](const auto& ops) {
        for (auto&& op : ops) {
            if (op.ident != indexBuildIdent) {
                continue;
            }
            const auto* keyPtr = std::get_if<int64_t>(&op.key);
            if (!keyPtr) {
                continue;
            }
            BSONObj bson{op.value.data()};
            if (*keyPtr == 1) {
                currentPhase =
                    IndexBuildMetadata::parse(bson, IDLParserContext{"IndexBuildMetadata"})
                        .getPhase();
            } else if (*keyPtr == 2 && currentPhase) {
                auto state = IndexStateInfo::parse(bson, IDLParserContext{"IndexStateInfo"});
                switch (*currentPhase) {
                    case IndexBuildPhaseEnum::kCollectionScan:
                        scanStates.push_back(std::move(state));
                        break;
                    case IndexBuildPhaseEnum::kBulkLoad:
                        loadStates.push_back(std::move(state));
                        break;
                    case IndexBuildPhaseEnum::kInitialized:
                    case IndexBuildPhaseEnum::kDrainWrites:
                        ADD_FAILURE()
                            << "Unexpected index build phase: " << idl::serialize(*currentPhase);
                        break;
                }
            }
        }
    };
    pairWrites(observer.inserts);
    pairWrites(observer.updates);

    EXPECT_FALSE(scanStates.empty());
    EXPECT_FALSE(loadStates.empty());

    // At least one scan-phase record must carry `lastSpilledRecordId` — the seed itself is
    // written when transitioning to the scan phase but before any spill, so it carries no
    // `lastSpilledRecordId`. Spills during the scan must have it set.
    EXPECT_TRUE(std::any_of(scanStates.begin(), scanStates.end(), [](const auto& state) {
        return state.getLastSpilledRecordId().has_value();
    }));
    for (const auto& state : loadStates) {
        EXPECT_FALSE(state.getLastSpilledRecordId());
    }

    indexer.abortIndexBuild(operationContext(), coll, MultiIndexBlock::kNoopOnCleanUpFn);
}
}  // namespace
}  // namespace mongo
