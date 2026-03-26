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
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
        lastAbortArgs = AbortArgs{.ns = ns,
                                  .collUUID = collUUID,
                                  .buildUUID = buildUUID,
                                  .indexes = indexes,
                                  .cause = cause,
                                  .fromMigrate = fromMigrate,
                                  .isTimeseries = isTimeseries};
    }

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
        ASSERT_FALSE(keys.empty());
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

    NamespaceString ns = NamespaceString::createNamespaceString_forTest("test.primary_driven");
    UUID collUUID = UUID::gen();

private:
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest _featureFlag{"featureFlagPrimaryDrivenIndexBuilds", true};
};

TEST_F(UtilTest, Start) {
    auto buildUUID = UUID::gen();
    auto indexes = makeIndexes({"a", "b"});

    ASSERT_OK(start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes));

    auto coll = acquireCollectionMaybeLockFree(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(),
                                                {ns.dbName(), collUUID},
                                                AcquisitionPrerequisites::OperationType::kRead));

    for (auto&& index : indexes) {
        auto entry = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
            operationContext(), index.getIndexName(), IndexCatalog::InclusionPolicy::kUnfinished);
        ASSERT_TRUE(entry);
        EXPECT_FALSE(entry->isReady());

        auto& engine = *operationContext()->getServiceContext()->getStorageEngine()->getEngine();
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        EXPECT_TRUE(engine.hasIdent(ru, index.indexIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.sorterIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.sideWritesIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.skippedRecordsIdent));
        EXPECT_TRUE(engine.hasIdent(ru, *index.constraintViolationsIdent));
    }

    ASSERT_TRUE(opObserver().lastStartArgs);
    auto& args = *opObserver().lastStartArgs;
    EXPECT_EQ(args.ns, ns);
    EXPECT_EQ(args.collUUID, collUUID);
    EXPECT_EQ(args.buildUUID, buildUUID);
    EXPECT_EQ(args.indexes.size(), indexes.size());
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

    ASSERT_OK(start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes));
    ASSERT_OK(commit(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, multikey));

    for (auto&& index : indexes) {
        auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
        ASSERT_OK(engine.immediatelyCompletePendingDrop(operationContext(), index.indexIdent));
        ASSERT_OK(engine.immediatelyCompletePendingDrop(operationContext(), *index.sorterIdent));
        ASSERT_OK(
            engine.immediatelyCompletePendingDrop(operationContext(), *index.sideWritesIdent));
        ASSERT_OK(
            engine.immediatelyCompletePendingDrop(operationContext(), *index.skippedRecordsIdent));
        ASSERT_OK(engine.immediatelyCompletePendingDrop(operationContext(),
                                                        *index.constraintViolationsIdent));
    }

    auto coll = acquireCollectionMaybeLockFree(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(),
                                                {ns.dbName(), collUUID},
                                                AcquisitionPrerequisites::OperationType::kRead));

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

        auto& engine = *operationContext()->getServiceContext()->getStorageEngine()->getEngine();
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        EXPECT_TRUE(engine.hasIdent(ru, indexes[i].indexIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sorterIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sideWritesIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].skippedRecordsIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].constraintViolationsIdent));
    }

    ASSERT_TRUE(opObserver().lastCommitArgs);
    auto& args = *opObserver().lastCommitArgs;
    EXPECT_EQ(args.ns, ns);
    EXPECT_EQ(args.collUUID, collUUID);
    EXPECT_EQ(args.buildUUID, buildUUID);
    EXPECT_EQ(args.indexes.size(), 2);
    ASSERT_BSONOBJ_EQ(args.indexes[0].spec, indexes[0].spec);
    ASSERT_BSONOBJ_EQ(args.indexes[1].spec, indexes[1].spec);
    ASSERT_FALSE(args.multikey[0]);
    ASSERT_TRUE(args.multikey[1]);
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

    ASSERT_OK(start(operationContext(), ns.dbName(), collUUID, buildUUID, indexes));
    ASSERT_OK(abort(operationContext(), ns.dbName(), collUUID, buildUUID, indexes, cause));

    for (auto&& index : indexes) {
        auto& engine = *operationContext()->getServiceContext()->getStorageEngine();
        ASSERT_OK(engine.immediatelyCompletePendingDrop(operationContext(), index.indexIdent));
        ASSERT_OK(engine.immediatelyCompletePendingDrop(operationContext(), *index.sorterIdent));
        ASSERT_OK(
            engine.immediatelyCompletePendingDrop(operationContext(), *index.sideWritesIdent));
        ASSERT_OK(
            engine.immediatelyCompletePendingDrop(operationContext(), *index.skippedRecordsIdent));
        ASSERT_OK(engine.immediatelyCompletePendingDrop(operationContext(),
                                                        *index.constraintViolationsIdent));
    }

    auto coll = acquireCollectionMaybeLockFree(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(operationContext(),
                                                {ns.dbName(), collUUID},
                                                AcquisitionPrerequisites::OperationType::kRead));

    for (size_t i = 0; i < indexes.size(); ++i) {
        ASSERT_FALSE(coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
            operationContext(), indexes[i].getIndexName(), IndexCatalog::InclusionPolicy::kReady));

        auto& engine = *operationContext()->getServiceContext()->getStorageEngine()->getEngine();
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        EXPECT_FALSE(engine.hasIdent(ru, indexes[i].indexIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sorterIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].sideWritesIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].skippedRecordsIdent));
        EXPECT_FALSE(engine.hasIdent(ru, *indexes[i].constraintViolationsIdent));
    }

    ASSERT_TRUE(opObserver().lastAbortArgs);
    auto& args = *opObserver().lastAbortArgs;
    EXPECT_EQ(args.ns, ns);
    EXPECT_EQ(args.collUUID, collUUID);
    EXPECT_EQ(args.buildUUID, buildUUID);
    EXPECT_EQ(args.indexes.size(), 2);
    ASSERT_BSONOBJ_EQ(args.indexes[0].spec, indexes[0].spec);
    ASSERT_BSONOBJ_EQ(args.indexes[1].spec, indexes[1].spec);
    ASSERT_EQ(args.cause, cause);
    EXPECT_FALSE(args.fromMigrate);
    EXPECT_FALSE(args.isTimeseries);
}

}  // namespace
}  // namespace mongo::index_builds::primary_driven
