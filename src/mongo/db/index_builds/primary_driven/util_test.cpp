/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/index_builds/primary_driven/util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/index_builds/resumable_index_builds_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::index_builds::primary_driven {
namespace {

class TestOpObserver : public OpObserverNoop {
public:
    struct StartArgs {
        NamespaceString ns;
        UUID collUUID;
        UUID buildUUID;
        std::vector<IndexBuildInfo> indexes;
        bool fromMigrate;
        bool isTimeseries;
    };

    struct CommitArgs {
        NamespaceString ns;
        UUID collUUID;
        UUID buildUUID;
        std::vector<IndexBuildInfo> indexes;
        std::vector<boost::optional<BSONObj>> multikey;
        bool fromMigrate;
        bool isTimeseries;
    };

    struct AbortArgs {
        NamespaceString ns;
        UUID collUUID;
        UUID buildUUID;
        std::vector<IndexBuildInfo> indexes;
        Status cause;
        bool fromMigrate;
        bool isTimeseries;
    };

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const UUID& collUUID,
                           const UUID& buildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isTimeseries) override {
        lastStartArgs = StartArgs{.ns = ns,
                                  .collUUID = collUUID,
                                  .buildUUID = buildUUID,
                                  .indexes = indexes,
                                  .fromMigrate = fromMigrate,
                                  .isTimeseries = isTimeseries};
    }

    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const UUID& collUUID,
                            const UUID& buildUUID,
                            const std::vector<IndexBuildInfo>& indexes,
                            const std::vector<boost::optional<BSONObj>>& multikey,
                            bool fromMigrate,
                            bool isTimeseries) override {
        if (throwOnCommit) {
            uasserted(ErrorCodes::InterruptedDueToReplStateChange, "simulated stepdown");
        }
        lastCommitArgs = CommitArgs{.ns = ns,
                                    .collUUID = collUUID,
                                    .buildUUID = buildUUID,
                                    .indexes = indexes,
                                    .multikey = multikey,
                                    .fromMigrate = fromMigrate,
                                    .isTimeseries = isTimeseries};
    }

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const UUID& collUUID,
                           const UUID& buildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           const Status& cause,
                           bool fromMigrate,
                           bool isTimeseries) override {
        if (throwOnAbort) {
            uasserted(ErrorCodes::InterruptedDueToReplStateChange, "simulated stepdown");
        }
        lastAbortArgs = AbortArgs{.ns = ns,
                                  .collUUID = collUUID,
                                  .buildUUID = buildUUID,
                                  .indexes = indexes,
                                  .cause = cause,
                                  .fromMigrate = fromMigrate,
                                  .isTimeseries = isTimeseries};
    }

    bool throwOnAbort = false;
    bool throwOnCommit = false;

    boost::optional<StartArgs> lastStartArgs;
    boost::optional<CommitArgs> lastCommitArgs;
    boost::optional<AbortArgs> lastAbortArgs;
};

class UtilTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        getServiceContext()->resetOpObserver_forTest(std::make_unique<TestOpObserver>());
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), ns, CollectionOptions{.uuid = collUUID}));
    }

    TestOpObserver& opObserver() {
        return *static_cast<TestOpObserver*>(getServiceContext()->getOpObserver());
    }

    std::vector<IndexBuildInfo> makeIndexes(const std::vector<std::string>& keys) {
        EXPECT_FALSE(keys.empty());
        std::vector<IndexBuildInfo> indexes;
        indexes.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            indexes.emplace_back(BSON(IndexDescriptor::kIndexVersionFieldName
                                      << IndexConfig::kLatestIndexVersion
                                      << IndexDescriptor::kKeyPatternFieldName << BSON(keys[i] << 1)
                                      << IndexDescriptor::kIndexNameFieldName << (keys[i] + "_1")
                                      << IndexDescriptor::kUniqueFieldName << true),
                                 fmt::format("index-{}", i));
            indexes.back().setInternalIdents(fmt::format("internal-sorter-{}", i),
                                             fmt::format("internal-sideWrites-{}", i),
                                             fmt::format("internal-skippedRecords-{}", i),
                                             fmt::format("internal-constraintViolations-{}", i));
        }
        return indexes;
    }

    std::unique_ptr<RecordStore> makeIndexBuildResumeTable(
        const std::string& ident, boost::optional<BSONObj> resumeStateData) {
        std::unique_ptr<RecordStore> ret;
        auto opCtx = operationContext();
        Lock::GlobalLock lk(opCtx, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        ret = opCtx->getServiceContext()->getStorageEngine()->makeInternalRecordStore(
            opCtx, ident, KeyFormat::Long);
        if (resumeStateData) {
            ASSERT_OK(ret->insertRecord(operationContext(),
                                        *shard_role_details::getRecoveryUnit(opCtx),
                                        resumeStateData->objdata(),
                                        resumeStateData->objsize(),
                                        Timestamp()));
        }
        wuow.commit();
        return ret;
    }

    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.primary_driven");
    UUID collUUID = UUID::gen();

private:
    // TODO (SERVER-116165): Remove.
    unittest::ServerParameterGuard _featureFlag{"featureFlagPrimaryDrivenIndexBuilds", true};
};

TEST_F(UtilTest, Start) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a", "b"});
    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);

    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));

    auto coll = acquireCollectionMaybeLockFree(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(),
                                                {ns.dbName(), collUUID},
                                                AcquisitionPrerequisites::OperationType::kRead));

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine()->getEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    for (auto&& index : indexes) {
        auto entry = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
            operationContext(), index.getIndexName(), IndexCatalog::InclusionPolicy::kUnfinished);
        ASSERT_TRUE(entry);
        EXPECT_FALSE(entry->isReady());

        EXPECT_TRUE(engine.hasIdent(ru, index.indexIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.sorterIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.sideWritesIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.skippedRecordsIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.constraintViolationsIdent));
    }
    EXPECT_TRUE(engine.hasIdent(ru, indexBuildIdent));

    ASSERT_TRUE(opObserver().lastStartArgs);
    auto& args = *opObserver().lastStartArgs;
    EXPECT_EQ(args.ns, ns);
    EXPECT_EQ(args.collUUID, collUUID);
    EXPECT_EQ(args.buildUUID, buildUUID);
    ASSERT_EQ(args.indexes.size(), indexes.size());
    for (size_t i = 0; i < indexes.size(); ++i) {
        ASSERT_BSONOBJ_EQ(args.indexes[i].spec, indexes[i].spec);
        EXPECT_EQ(args.indexes[i].indexIdent, indexes[i].indexIdent);
        EXPECT_EQ(args.indexes[i].sorterIdent, indexes[i].sorterIdent);
        EXPECT_EQ(args.indexes[i].sideWritesIdent, indexes[i].sideWritesIdent);
        EXPECT_EQ(args.indexes[i].skippedRecordsIdent, indexes[i].skippedRecordsIdent);
        EXPECT_EQ(args.indexes[i].constraintViolationsIdent, indexes[i].constraintViolationsIdent);
    }
    EXPECT_FALSE(args.fromMigrate);
    EXPECT_FALSE(args.isTimeseries);
}

TEST_F(UtilTest, Commit) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a", "b"});

    std::vector<boost::optional<MultikeyPaths>> multikey(indexes.size());
    multikey[1].emplace();
    multikey[1]->emplace_back();
    (*multikey[1])[0].insert(0);

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));
    const Timestamp commitTs(1, 0);
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(commitTs);
    ASSERT_OK(commit(
        operationContext(), ns.dbName(), collUUID, buildUUID, indexes, multikey, indexBuildIdent));

    const Timestamp dropTs(commitTs.getSecs() + 1, 0);
    for (auto&& index : indexes) {
        auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
        engine.dropIdentTimestamped(operationContext(), *index.sorterIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.sideWritesIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.skippedRecordsIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.constraintViolationsIdent, dropTs);
    }
    operationContext()->getServiceContext()->getStorageEngine()->dropIdentTimestamped(
        operationContext(), indexBuildIdent, dropTs);

    auto coll = acquireCollectionMaybeLockFree(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(),
                                                {ns.dbName(), collUUID},
                                                AcquisitionPrerequisites::OperationType::kRead));

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine()->getEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    for (size_t i = 0; i < indexes.size(); ++i) {
        auto entry = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
            operationContext(), indexes[i].getIndexName(), IndexCatalog::InclusionPolicy::kReady);
        ASSERT_TRUE(entry);
        EXPECT_TRUE(entry->isReady());
        if (multikey[i]) {
            EXPECT_TRUE(entry->isMultikey(operationContext(), coll.getCollectionPtr()));
        } else {
            EXPECT_FALSE(entry->isMultikey(operationContext(), coll.getCollectionPtr()));
        }

        EXPECT_TRUE(engine.hasIdent(ru, indexes[i].indexIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sorterIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sideWritesIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].skippedRecordsIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].constraintViolationsIdent));
    }
    EXPECT_FALSE(engine.hasIdent(ru, indexBuildIdent));

    // Verify indexes are present in the tracker after commit.
    const auto usageStats =
        CollectionIndexUsageTrackerDecoration::getUsageStats(coll.getCollectionPtr().get());
    for (auto&& index : indexes) {
        EXPECT_TRUE(usageStats.count(index.getIndexName()));
    }

    ASSERT_TRUE(opObserver().lastCommitArgs);
    auto& args = *opObserver().lastCommitArgs;
    EXPECT_EQ(args.ns, ns);
    EXPECT_EQ(args.collUUID, collUUID);
    EXPECT_EQ(args.buildUUID, buildUUID);
    ASSERT_EQ(args.indexes.size(), 2);
    ASSERT_BSONOBJ_EQ(args.indexes[0].spec, indexes[0].spec);
    ASSERT_BSONOBJ_EQ(args.indexes[1].spec, indexes[1].spec);
    EXPECT_FALSE(args.multikey[0]);
    EXPECT_TRUE(args.multikey[1]);
    EXPECT_FALSE(args.fromMigrate);
    EXPECT_FALSE(args.isTimeseries);
}

TEST_F(UtilTest, Abort) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a", "b"});
    Status cause{ErrorCodes::Error{11130401}, "abort"};

    std::vector<boost::optional<MultikeyPaths>> multikey(indexes.size());
    multikey[1].emplace();
    multikey[1]->emplace_back();
    (*multikey[1])[0].insert(0);

    const auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));
    const Timestamp commitTs(1, 0);
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(commitTs);
    ASSERT_OK(abort(
        operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent, cause));

    const Timestamp dropTs(commitTs.getSecs() + 1, 0);
    for (auto&& index : indexes) {
        auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
        engine.dropIdentTimestamped(operationContext(), index.indexIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.sorterIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.sideWritesIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.skippedRecordsIdent, dropTs);
        engine.dropIdentTimestamped(operationContext(), *index.constraintViolationsIdent, dropTs);
    }
    operationContext()->getServiceContext()->getStorageEngine()->dropIdentTimestamped(
        operationContext(), indexBuildIdent, dropTs);

    auto coll = acquireCollectionMaybeLockFree(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(),
                                                {ns.dbName(), collUUID},
                                                AcquisitionPrerequisites::OperationType::kRead));

    auto& engine = *operationContext()->getServiceContext()->getStorageEngine()->getEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    for (size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_FALSE(coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
            operationContext(), indexes[i].getIndexName(), IndexCatalog::InclusionPolicy::kReady));

        EXPECT_FALSE(engine.hasIdent(ru, indexes[i].indexIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sorterIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sideWritesIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].skippedRecordsIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].constraintViolationsIdent));
    }
    EXPECT_FALSE(engine.hasIdent(ru, indexBuildIdent));

    ASSERT_TRUE(opObserver().lastAbortArgs);
    auto& args = *opObserver().lastAbortArgs;
    EXPECT_EQ(args.ns, ns);
    EXPECT_EQ(args.collUUID, collUUID);
    EXPECT_EQ(args.buildUUID, buildUUID);
    ASSERT_EQ(args.indexes.size(), 2);
    ASSERT_BSONOBJ_EQ(args.indexes[0].spec, indexes[0].spec);
    ASSERT_BSONOBJ_EQ(args.indexes[1].spec, indexes[1].spec);
    EXPECT_EQ(args.cause, cause);
    EXPECT_FALSE(args.fromMigrate);
    EXPECT_FALSE(args.isTimeseries);
}

TEST_F(UtilTest, CommitUsesCommitTimestampForTemporaryTableDrops) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a"});
    std::vector<boost::optional<MultikeyPaths>> multikey(indexes.size());
    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);

    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));

    const Timestamp commitTs(200, 0);
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(commitTs);
    ASSERT_OK(commit(
        operationContext(), ns.dbName(), collUUID, buildUUID, indexes, multikey, indexBuildIdent));

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    for (auto&& index : indexes) {
        // The drop was registered at the commit timestamp. A timestamp <= commitTs should fail.
        ASSERT_THROWS_CODE(storageEngine->dropIdentTimestamped(
                               operationContext(), *index.sideWritesIdent, commitTs),
                           DBException,
                           ErrorCodes::ObjectIsBusy);

        // A timestamp greater than commitTs should succeed.
        storageEngine->dropIdentTimestamped(
            operationContext(), *index.sideWritesIdent, Timestamp(commitTs.getSecs() + 1, 0));
    }
}

TEST_F(UtilTest, AbortUsesCommitTimestampForTemporaryTableDrops) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a"});
    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    Status cause{ErrorCodes::Error{11130402}, "abort"};

    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));

    const Timestamp commitTs(300, 0);
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(commitTs);
    ASSERT_OK(abort(
        operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent, cause));

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    for (auto&& index : indexes) {
        ASSERT_THROWS_CODE(storageEngine->dropIdentTimestamped(
                               operationContext(), *index.sideWritesIdent, commitTs),
                           DBException,
                           ErrorCodes::ObjectIsBusy);

        storageEngine->dropIdentTimestamped(
            operationContext(), *index.sideWritesIdent, Timestamp(commitTs.getSecs() + 1, 0));
    }
}


TEST_F(UtilTest, AbortWUOWRollbackAllowsRetry) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a"});
    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    const Status cause{ErrorCodes::Error{11130404}, "abort"};

    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));

    // Throw from the OpObserver to roll back the WUoW after dropIdentsAndDeregisterOnCommit
    // has registered its onCommit handler but before wuow.commit().
    opObserver().throwOnAbort = true;
    ASSERT_THROWS_CODE(
        abort(
            operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent, cause),
        DBException,
        ErrorCodes::InterruptedDueToReplStateChange);
    opObserver().throwOnAbort = false;

    ASSERT_FALSE(registry(getServiceContext()).all().empty());
    EXPECT_EQ(getServiceContext()->getStorageEngine()->getNumDropPendingIdents(), 0U);
    // No mangled state.
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(Timestamp(1, 0));
    ASSERT_OK(abort(
        operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent, cause));
}

TEST_F(UtilTest, CommitWUOWRollbackAllowsRetry) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a"});
    auto indexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    std::vector<boost::optional<MultikeyPaths>> multikey(indexes.size());

    ASSERT_OK(
        start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, indexBuildIdent));

    // Throw from the OpObserver to roll back the WUoW after dropIdentsAndDeregisterOnCommit
    // has registered its onCommit handler but before wuow.commit().
    opObserver().throwOnCommit = true;
    ASSERT_THROWS_CODE(commit(operationContext(),
                              ns.dbName(),
                              collUUID,
                              buildUUID,
                              indexes,
                              multikey,
                              indexBuildIdent),
                       DBException,
                       ErrorCodes::InterruptedDueToReplStateChange);
    opObserver().throwOnCommit = false;

    ASSERT_FALSE(registry(getServiceContext()).all().empty());
    EXPECT_EQ(getServiceContext()->getStorageEngine()->getNumDropPendingIdents(), 0U);
    // No mangled state.
    shard_role_details::getRecoveryUnit(operationContext())->setCommitTimestamp(Timestamp(1, 0));
    ASSERT_OK(commit(
        operationContext(), ns.dbName(), collUUID, buildUUID, indexes, multikey, indexBuildIdent));
}

TEST_F(UtilTest, ResumeInfoRequiresValidIdent) {
    auto buildUUID = UUID::gen();
    std::vector<IndexBuildInfo> indexes;
    std::vector<std::string> invalidIdents = {
        std::string(""),
        fmt::format("some-invalid-{}", buildUUID.toString()),
        ident::generateNewInternalIdent(kResumableIndexIdentStem)};
    for (auto&& testIdent : invalidIdents) {
        ASSERT_THROWS_CODE(resumeInfo(operationContext(), collUUID, buildUUID, indexes, testIdent),
                           DBException,
                           ErrorCodes::InvalidOptions);
    }
}

TEST_F(UtilTest, ResumeInfoFailsOnParseError) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a"});
    auto validResumableIndexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);

    auto invalidResumeState = BSONObjBuilder{}.append("foo", BSON("bar" << "baz")).obj();
    makeIndexBuildResumeTable(validResumableIndexBuildIdent, invalidResumeState);

    ASSERT_THROWS_CODE(
        resumeInfo(operationContext(), collUUID, buildUUID, indexes, validResumableIndexBuildIdent),
        DBException,
        ErrorCodes::FailedToParse);
}

TEST_F(UtilTest, ResumeInfoSynthesizesMissingRecord) {
    auto buildUUID = UUID::gen();
    auto validResumableIndexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    auto indexes = makeIndexes({"a", "b", "c"});
    makeIndexBuildResumeTable(validResumableIndexBuildIdent, boost::none);

    auto resumeState =
        resumeInfo(operationContext(), collUUID, buildUUID, indexes, validResumableIndexBuildIdent);
    EXPECT_EQ(buildUUID, resumeState.getBuildUUID());
    EXPECT_EQ(IndexBuildPhaseEnum::kInitialized, resumeState.getPhase());
    EXPECT_EQ(collUUID, resumeState.getCollectionUUID());
    for (size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(*indexes[i].sideWritesIdent, resumeState.getIndexes()[i].getSideWritesTable());
        EXPECT_EQ(*indexes[i].constraintViolationsIdent,
                  *resumeState.getIndexes()[i].getDuplicateKeyTrackerTable());
        EXPECT_EQ(*indexes[i].skippedRecordsIdent,
                  *resumeState.getIndexes()[i].getSkippedRecordTrackerTable());
        EXPECT_EQ(*indexes[i].sorterIdent, *resumeState.getIndexes()[i].getStorageIdentifier());
    }
}

TEST_F(UtilTest, ResumeInfoParsesSuccessfully) {
    auto buildUUID = UUID::gen();
    auto validResumableIndexBuildIdent = ident::generateNewIndexBuildIdent(buildUUID);
    auto indexes = makeIndexes({"a", "b", "c"});

    auto resumeStateBSONObj =
        index_builds::synthesizeResumeIndexInfo(
            buildUUID, IndexBuildPhaseEnum::kCollectionScan, collUUID, indexes)
            .toBSON();
    makeIndexBuildResumeTable(validResumableIndexBuildIdent, resumeStateBSONObj);

    auto resumeState =
        resumeInfo(operationContext(), collUUID, buildUUID, indexes, validResumableIndexBuildIdent);
    EXPECT_EQ(buildUUID, resumeState.getBuildUUID());
    EXPECT_EQ(IndexBuildPhaseEnum::kCollectionScan, resumeState.getPhase());
    EXPECT_EQ(collUUID, resumeState.getCollectionUUID());
    for (size_t i = 0; i < indexes.size(); ++i) {
        EXPECT_EQ(*indexes[i].sideWritesIdent, resumeState.getIndexes()[i].getSideWritesTable());
        EXPECT_EQ(*indexes[i].constraintViolationsIdent,
                  *resumeState.getIndexes()[i].getDuplicateKeyTrackerTable());
        EXPECT_EQ(*indexes[i].skippedRecordsIdent,
                  *resumeState.getIndexes()[i].getSkippedRecordTrackerTable());
        EXPECT_EQ(*indexes[i].sorterIdent, *resumeState.getIndexes()[i].getStorageIdentifier());
    }
}

std::vector<int64_t> getSorterKeys(OperationContext* opCtx, IntegerKeyedContainer& container) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto cursor = container.getCursor(ru);
    std::vector<int64_t> keys;
    while (auto entry = cursor->next()) {
        keys.push_back(entry->first);
    }
    return keys;
}

void insertSorterEntries(OperationContext* opCtx,
                         IntegerKeyedContainer& container,
                         int64_t startKey,
                         int64_t endKey) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    Lock::GlobalLock lk(opCtx, MODE_IX);
    WriteUnitOfWork wuow{opCtx};
    const char dummyValue[] = "value";
    for (int64_t key = startKey; key < endKey; ++key) {
        ASSERT_OK(container_write::insert(opCtx,
                                          ru,
                                          container,
                                          key,
                                          std::span<const char>(dummyValue, sizeof(dummyValue)),
                                          container_write::NonexistentKeyGuarantee{}));
    }
    wuow.commit();
}

TEST_F(UtilTest, DeleteSorterEntriesOutsideRangesDeletesOutOfRangeKeys) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-delete-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    insertSorterEntries(opCtx, container, 1, 11);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    SorterRange range;
    range.setStart(3);
    range.setEnd(8);
    range.setChecksum(0);
    indexInfo.setRanges(std::vector<SorterRange>{range});

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_EQ(remainingKeys.size(), 5u);
    for (int64_t expected = 3; expected < 8; ++expected) {
        EXPECT_TRUE(std::find(remainingKeys.begin(), remainingKeys.end(), expected) !=
                    remainingKeys.end());
    }
}

TEST_F(UtilTest, DeleteSorterEntriesOutsideRangesDeletesAllWhenRangesUnset) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-unset-ranges-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    insertSorterEntries(opCtx, container, 1, 6);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    // Ranges are left unset, so all entries are orphaned.

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_TRUE(remainingKeys.empty());
}

TEST_F(UtilTest, DeleteSorterEntriesOutsideRangesDeletesAllWhenRangesEmpty) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-empty-ranges-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    insertSorterEntries(opCtx, container, 1, 6);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    // Ranges are present but empty, so all entries are orphaned.
    indexInfo.setRanges(std::vector<SorterRange>{});

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_TRUE(remainingKeys.empty());
}

TEST_F(UtilTest, DeleteSorterEntriesOutsideRangesNoOpWhenAllWithinRange) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-within-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    insertSorterEntries(opCtx, container, 1, 6);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    SorterRange range;
    range.setStart(1);
    range.setEnd(6);
    range.setChecksum(0);
    indexInfo.setRanges(std::vector<SorterRange>{range});

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_EQ(remainingKeys.size(), 5u);
}

TEST_F(UtilTest, DeleteSorterEntriesOutsideRangesDeletesAcrossBatches) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    // Set a small batch size to force multiple delete batches.
    unittest::ServerParameterGuard batchSize{"primaryDrivenIndexBuildSorterInsertionBatchSize", 3};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-batch-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    insertSorterEntries(opCtx, container, 1, 16);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    SorterRange range;
    range.setStart(1);
    range.setEnd(6);
    range.setChecksum(0);
    indexInfo.setRanges(std::vector<SorterRange>{range});

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_EQ(remainingKeys.size(), 5u);
    for (int64_t expected = 1; expected < 6; ++expected) {
        EXPECT_TRUE(std::find(remainingKeys.begin(), remainingKeys.end(), expected) !=
                    remainingKeys.end());
    }
}

// Fires a deterministic WCE while removing keys < firstStart.
TEST_F(UtilTest, DeleteSorterEntriesOutsideRangesSurvivesWCEWhenDeletingKeysLessThanFirstStart) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard batchSize{"primaryDrivenIndexBuildSorterInsertionBatchSize", 5};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-wce-pre-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    // We will be deleting keys < 6 and >= 11.
    insertSorterEntries(opCtx, container, 1, 6);
    insertSorterEntries(opCtx, container, 6, 11);
    insertSorterEntries(opCtx, container, 11, 16);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    SorterRange range;
    range.setStart(6);
    range.setEnd(11);
    range.setChecksum(0);
    indexInfo.setRanges(std::vector<SorterRange>{range});

    auto failPoint = enableWriteConflictForWrites(
        FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
    const auto initialTimesEntered = failPoint->initialTimesEntered();

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    // Exactly one WCE must have fired when removing keys < 6.
    EXPECT_EQ(initialTimesEntered + 1, (*failPoint)->waitForTimesEntered(initialTimesEntered + 1));

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_EQ(remainingKeys.size(), 5u);
    for (int64_t expected = 6; expected < 11; ++expected) {
        EXPECT_TRUE(std::find(remainingKeys.begin(), remainingKeys.end(), expected) !=
                    remainingKeys.end());
    }
}

// Fires a deterministic WCE while removing keys >= LastEnd.
TEST_F(UtilTest,
       DeleteSorterEntriesOutsideRangesSurvivesWCEWhenDeletingKeysGreaterThanOrEqualToLastEnd) {
    unittest::ServerParameterGuard containerWritesEnabled{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard batchSize{"primaryDrivenIndexBuildSorterInsertionBatchSize", 4};
    static_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()))
        ->alwaysAllowWrites(true);

    auto opCtx = operationContext();
    std::string sorterIdent = "internal-sorter-wce-post-test";
    LazyRecordStore sorterTable(opCtx, sorterIdent, LazyRecordStore::CreateMode::immediate);
    auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                          sorterTable.getTableOrThrow().getContainer())
                          .get();

    // We will be deleting keys >= 11.
    insertSorterEntries(opCtx, container, 6, 11);
    insertSorterEntries(opCtx, container, 11, 17);

    IndexStateInfo indexInfo;
    indexInfo.setSpec(BSON("key" << BSON("a" << 1) << "name"
                                 << "a_1"
                                 << "v" << IndexConfig::kLatestIndexVersion));
    indexInfo.setIsMultikey(false);
    indexInfo.setMultikeyPaths({});
    indexInfo.setStorageIdentifier(sorterIdent);
    SorterRange range;
    range.setStart(6);
    range.setEnd(11);
    range.setChecksum(0);
    indexInfo.setRanges(std::vector<SorterRange>{range});

    auto failPoint = enableWriteConflictForWrites(
        FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
    const auto initialTimesEntered = failPoint->initialTimesEntered();

    deleteSorterEntriesOutsideRanges(opCtx, {indexInfo});

    // Exactly one WCE must have fired when removing keys >= 11.
    EXPECT_EQ(initialTimesEntered + 1, (*failPoint)->waitForTimesEntered(initialTimesEntered + 1));

    auto remainingKeys = getSorterKeys(opCtx, container);
    EXPECT_EQ(remainingKeys.size(), 5u);
    for (int64_t expected = 6; expected < 11; ++expected) {
        EXPECT_TRUE(std::find(remainingKeys.begin(), remainingKeys.end(), expected) !=
                    remainingKeys.end());
    }
}

}  // namespace
}  // namespace mongo::index_builds::primary_driven
