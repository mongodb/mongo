// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_builds_manager.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/primary_driven/registry.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/ident.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo {
namespace {

class IndexBuildsManagerTest : public CatalogTestFixture {
private:
    void setUp() override;
    void tearDown() override;

public:
    void createCollection(const NamespaceString& nss);

    UUID getCollectionUUID() const;

    std::vector<IndexBuildInfo> makeSpecs(std::vector<std::string> keys);

    const UUID _buildUUID = UUID::gen();
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.foo");
    IndexBuildsManager _indexBuildsManager;

private:
    unittest::ServerParameterGuard _featureFlag{"featureFlagPrimaryDrivenIndexBuilds", true};
};

void IndexBuildsManagerTest::setUp() {
    CatalogTestFixture::setUp();
    createCollection(_nss);
}

void IndexBuildsManagerTest::tearDown() {
    _indexBuildsManager.verifyNoIndexBuilds_forTestOnly();
    // All databases are dropped during tear down.
    CatalogTestFixture::tearDown();
}

void IndexBuildsManagerTest::createCollection(const NamespaceString& nss) {
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
}

UUID IndexBuildsManagerTest::getCollectionUUID() const {
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), _nss, AcquisitionPrerequisites::kRead),
                                 MODE_IS);
    return acq.uuid();
}

std::vector<IndexBuildInfo> IndexBuildsManagerTest::makeSpecs(std::vector<std::string> keys) {
    ASSERT(keys.size());
    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
    std::vector<IndexBuildInfo> indexes;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& keyName = keys[i];
        IndexBuildInfo indexBuildInfo(
            BSON("v" << 2 << "key" << BSON(keyName << 1) << "name" << (keyName + "_1")),
            fmt::format("index-{}", i + 1),
            *storageEngine);
        indexes.push_back(std::move(indexBuildInfo));
    }
    return indexes;
}

TEST_F(IndexBuildsManagerTest, IndexBuildsManagerSetUpAndTearDown) {
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), _nss, AcquisitionPrerequisites::kWrite),
                                 MODE_X);
    CollectionWriter collection(operationContext(), &acq);

    auto indexes = makeSpecs({"a", "b"});
    ASSERT_OK(_indexBuildsManager.setUpIndexBuild(
        operationContext(), collection, indexes, _buildUUID, MultiIndexBlock::kNoopOnInitFn));

    _indexBuildsManager.abortIndexBuild(
        operationContext(), collection, _buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    _indexBuildsManager.tearDownAndUnregisterIndexBuild(_buildUUID);
}

TEST_F(IndexBuildsManagerTest, SetUpPrimaryDrivenIndexBuildAddsToRegistry) {
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), _nss, AcquisitionPrerequisites::kWrite),
                                 MODE_X);
    CollectionWriter collection{operationContext(), &acq};

    auto indexes = makeSpecs({"a", "b"});

    IndexBuildsManager::SetupOptions options;
    options.protocol = IndexBuildProtocol::kPrimaryDriven;
    options.method = IndexBuildMethodEnum::kPrimaryDriven;

    ASSERT_OK(_indexBuildsManager.setUpIndexBuild(operationContext(),
                                                  collection,
                                                  indexes,
                                                  _buildUUID,
                                                  MultiIndexBlock::kNoopOnInitFn,
                                                  options));

    auto entries =
        index_builds::primary_driven::registry(operationContext()->getServiceContext()).all();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].first, _buildUUID);
    EXPECT_EQ(entries[0].second.dbName, _nss.dbName());
    EXPECT_EQ(entries[0].second.collectionUUID, collection->uuid());
    EXPECT_EQ(entries[0].second.indexes.size(), indexes.size());
    for (size_t i = 0; i < indexes.size(); ++i) {
        ASSERT_BSONOBJ_EQ(entries[0].second.indexes[i].spec, indexes[i].spec);
    }
    ASSERT(entries[0].second.indexBuildIdent);
    EXPECT_EQ(*entries[0].second.indexBuildIdent, ident::generateNewIndexBuildIdent(_buildUUID));

    _indexBuildsManager.abortIndexBuild(
        operationContext(), collection, _buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    _indexBuildsManager.tearDownAndUnregisterIndexBuild(_buildUUID);
}

TEST_F(IndexBuildsManagerTest, SetUpFailureDoesNotAddPrimaryDrivenIndexBuildToRegistry) {
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), _nss, AcquisitionPrerequisites::kWrite),
                                 MODE_X);
    CollectionWriter collection{operationContext(), &acq};

    auto indexes = makeSpecs({"a"});

    IndexBuildsManager::SetupOptions options;
    options.protocol = IndexBuildProtocol::kPrimaryDriven;
    options.method = IndexBuildMethodEnum::kPrimaryDriven;

    auto throwingOnInit = []() {
        uasserted(ErrorCodes::InternalError, "simulated setUpIndexBuild failure");
    };

    ASSERT_NOT_OK(_indexBuildsManager.setUpIndexBuild(
        operationContext(), collection, indexes, _buildUUID, throwingOnInit, options));

    EXPECT_TRUE(index_builds::primary_driven::registry(operationContext()->getServiceContext())
                    .all()
                    .empty());

    _indexBuildsManager.tearDownAndUnregisterIndexBuild(_buildUUID);
}

TEST_F(IndexBuildsManagerTest, SetUpNonPrimaryDrivenIndexBuildDoesNotAddToRegistry) {
    auto acq = acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), _nss, AcquisitionPrerequisites::kWrite),
                                 MODE_X);
    CollectionWriter collection{operationContext(), &acq};

    auto indexes = makeSpecs({"a"});
    ASSERT_OK(_indexBuildsManager.setUpIndexBuild(
        operationContext(), collection, indexes, _buildUUID, MultiIndexBlock::kNoopOnInitFn));

    EXPECT_TRUE(index_builds::primary_driven::registry(operationContext()->getServiceContext())
                    .all()
                    .empty());

    _indexBuildsManager.abortIndexBuild(
        operationContext(), collection, _buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    _indexBuildsManager.tearDownAndUnregisterIndexBuild(_buildUUID);
}

// `MultiIndexBlock::commit`'s per-index loop collects multikey paths and passes them to `onCommit`;
// that collection must be scoped to a single attempt so a WCE retry doesn't leave the caller
// observing entries from a prior failed attempt. We force a retry by throwing a
// WriteConflictException from `onCommitFn` after the loop has completed all push_backs.
TEST_F(IndexBuildsManagerTest, CommitIndexBuildMultikeysAreResetWhenWceFiresAfterLoop) {
    auto opCtx = operationContext();
    const auto collectionUUID = getCollectionUUID();

    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, _nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    CollectionWriter collection(opCtx, &acq);

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(
            Helpers::insert(opCtx, collection.get(), BSON("_id" << 0 << "a" << 1 << "b" << 2)));
        wuow.commit();
    }

    auto indexes = makeSpecs({"a", "b"});
    ASSERT_OK(_indexBuildsManager.setUpIndexBuild(
        opCtx, collection, indexes, _buildUUID, MultiIndexBlock::kNoopOnInitFn));
    ASSERT_OK(
        _indexBuildsManager.startBuildingIndex(opCtx, _nss.dbName(), collectionUUID, _buildUUID));
    ASSERT_OK(_indexBuildsManager.drainBackgroundWrites(
        opCtx,
        _buildUUID,
        RecoveryUnit::ReadSource::kNoTimestamp,
        IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    int commitAttempts = 0;
    size_t multikeysAtCommit = 0;
    auto onCommitFn = [&](const std::vector<boost::optional<MultikeyPaths>>& multikeys) {
        ++commitAttempts;
        multikeysAtCommit = multikeys.size();
        if (commitAttempts == 1) {
            // Force a retry after the loop has populated `multikeys` so we can verify that the
            // next attempt observes a freshly-built vector rather than accumulated entries.
            throwWriteConflictException("Force WCE in onCommitFn after commit loop.");
        }
    };

    ASSERT_OK(_indexBuildsManager.commitIndexBuild(
        opCtx, collection, _nss, _buildUUID, MultiIndexBlock::kNoopOnCreateEachFn, onCommitFn));

    ASSERT_EQ(commitAttempts, 2);
    ASSERT_EQ(multikeysAtCommit, indexes.size());

    _indexBuildsManager.tearDownAndUnregisterIndexBuild(_buildUUID);
}
}  // namespace

}  // namespace mongo
