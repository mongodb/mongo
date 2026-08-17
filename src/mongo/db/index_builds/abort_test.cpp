// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/abort.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/primary_driven/registry.h"
#include "mongo/db/index_builds/primary_driven/util.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

namespace mongo::index_builds {
namespace {

class AbortTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        auto opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(std::make_unique<OpObserverNoop>());

        ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, {}));
        _collectionUUID = acquireExclusive().uuid();
    }

    CollectionAcquisition acquireExclusive(const NamespaceString& nss) {
        return acquireCollection(operationContext(),
                                 CollectionAcquisitionRequest::fromOpCtx(
                                     operationContext(), nss, AcquisitionPrerequisites::kWrite),
                                 MODE_X);
    }

    CollectionAcquisition acquireExclusive() {
        return acquireExclusive(_nss);
    }

    UUID createCollection(const NamespaceString& nss) {
        ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, {}));
        return acquireExclusive(nss).uuid();
    }

    // Refetches the Collection from the catalog in order to see any changes made to it.
    ConsistentCollection lookupCollection(const NamespaceString& nss) {
        return CollectionCatalog::get(operationContext())
            ->establishConsistentCollection(operationContext(), nss, boost::none);
    }

    int numIndexesInProgress(const NamespaceString& nss) {
        return lookupCollection(nss)->getIndexCatalog()->numIndexesInProgress();
    }

    int numIndexesInProgress() {
        return numIndexesInProgress(_nss);
    }

    int numIndexesTotal(const NamespaceString& nss) {
        return lookupCollection(nss)->getIndexCatalog()->numIndexesTotal();
    }

    index_builds::primary_driven::Registry& registry() {
        return index_builds::primary_driven::registry(getServiceContext());
    }

    /**
     * Starts a primary-driven index build on 'nss' without handing it to the
     * IndexBuildsCoordinator, building one index per entry of 'indexedFields'.
     */
    UUID startUnresumedPrimaryDrivenIndexBuild(
        const NamespaceString& nss,
        const UUID& collectionUUID,
        const std::vector<std::string>& indexedFields = {"a"},
        boost::optional<std::string> indexBuildIdent = boost::none) {
        std::vector<IndexBuildInfo> indexes;
        for (const auto& field : indexedFields) {
            indexes.emplace_back(
                BSON("v" << 2 << "key" << BSON(field << 1) << "name" << (field + "_1")),
                str::stream() << "index-" << _nextIndexIdent++,
                *getServiceContext()->getStorageEngine());
        }
        auto buildUUID = UUID::gen();
        ASSERT_OK(index_builds::primary_driven::start(operationContext(),
                                                      nss.dbName(),
                                                      collectionUUID,
                                                      buildUUID,
                                                      indexes,
                                                      std::move(indexBuildIdent)));
        return buildUUID;
    }

    UUID startUnresumedPrimaryDrivenIndexBuild() {
        return startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID);
    }

    void insertDocument() {
        auto collection = acquireExclusive();
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(
            operationContext(), collection.getCollectionPtr(), BSON("_id" << 1 << "a" << 1)));
        wuow.commit();
    }

    /**
     * Starts a primary-driven index build and hands it to the IndexBuildsCoordinator, which runs it
     * on a builder thread. Returns the future for the build's completion.
     */
    SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats> startCoordinatorRunIndexBuild() {
        std::vector<IndexBuildInfo> indexes;
        indexes.emplace_back(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"),
                             str::stream() << "index-" << _nextIndexIdent++,
                             *getServiceContext()->getStorageEngine());
        auto buildUUID = UUID::gen();
        registry().add(buildUUID, _nss.dbName(), _collectionUUID, indexes, boost::none);
        return unittest::assertGet(
            IndexBuildsCoordinator::get(operationContext())
                ->startIndexBuild(
                    operationContext(),
                    _nss.dbName(),
                    _collectionUUID,
                    indexes,
                    buildUUID,
                    {.indexBuildMethod = IndexBuildMethodEnum::kPrimaryDriven,
                     .indexBuildProtocol = IndexBuildProtocol::kPrimaryDriven,
                     .commitQuorum = CommitQuorumOptions{CommitQuorumOptions::kPrimarySelfVote}}));
    }

    /**
     * Returns a scope guard that sets a commit timestamp to use for abort.
     */
    auto makeCommitTimestampGuard() {
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        ru.setCommitTimestamp(Timestamp(1, 1));
        return ScopeGuard([&ru] { ru.clearCommitTimestamp(); });
    }

    NamespaceString _nss = NamespaceString::createNamespaceString_forTest("AbortTest.coll");
    UUID _collectionUUID = UUID::gen();
    int _nextIndexIdent = 1;
};

TEST_F(AbortTest, AbortsUnresumedPrimaryDrivenBuild) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild();

    ASSERT_EQ(registry().all().size(), 1);
    ASSERT_EQ(numIndexesInProgress(), 1);
    ASSERT_FALSE(IndexBuildsCoordinator::get(opCtx)->inProgForCollection(_collectionUUID));

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] { collection = boost::none; },
        [&] {
            collection.emplace(acquireExclusive());
            return true;
        });

    ASSERT_TRUE(collection.has_value());
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
}

TEST_F(AbortTest, AbortsUnresumedPrimaryDrivenBuildForWholeDatabase) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild();

    ASSERT_EQ(registry().all().size(), 1);
    ASSERT_EQ(numIndexesInProgress(), 1);
    ASSERT_FALSE(IndexBuildsCoordinator::get(opCtx)->inProgForDb(_nss.dbName()));

    boost::optional<Lock::DBLock> dbLock;
    dbLock.emplace(opCtx, _nss.dbName(), MODE_X);
    auto commitTimestampGuard = makeCommitTimestampGuard();
    abort(
        opCtx,
        _nss.dbName(),
        "database is being dropped",
        [&] { dbLock = boost::none; },
        [&] {
            dbLock.emplace(opCtx, _nss.dbName(), MODE_X);
            return true;
        });

    ASSERT_TRUE(dbLock.has_value());
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
}

TEST_F(AbortTest, LeavesIndexBuildsInOtherDatabasesAlone) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild();
    ASSERT_EQ(registry().all().size(), 1);

    const auto otherDbName =
        DatabaseName::createDatabaseName_forTest(boost::none, "someOtherDatabase");
    boost::optional<Lock::DBLock> dbLock;
    dbLock.emplace(opCtx, otherDbName, MODE_X);
    abort(
        opCtx,
        otherDbName,
        "database is being dropped",
        [&] { dbLock = boost::none; },
        [&] {
            dbLock.emplace(opCtx, otherDbName, MODE_X);
            return true;
        });

    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(), 1);
}

TEST_F(AbortTest, AbortsAllIndexesOfAnUnresumedPrimaryDrivenBuild) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"a", "b"});
    ASSERT_EQ(numIndexesInProgress(), 2);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    index_builds::abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] { collection = boost::none; },
        [&] {
            collection.emplace(acquireExclusive());
            return true;
        });

    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    // The collection and its '_id_' index are left behind.
    EXPECT_EQ(numIndexesTotal(_nss), 1);
}

TEST_F(AbortTest, AbortsUnresumedPrimaryDrivenBuildWithIndexBuildIdent) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    std::string indexBuildIdent = ident::generateNewIndexBuildIdent(UUID::gen());
    startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"a"}, indexBuildIdent);
    ASSERT_EQ(numIndexesInProgress(), 1);
    auto numDropPendingIdentsBefore =
        getServiceContext()->getStorageEngine()->getNumDropPendingIdents();

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    index_builds::abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] { collection = boost::none; },
        [&] {
            collection.emplace(acquireExclusive());
            return true;
        });

    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    // The build's internal tables are pending drop.
    EXPECT_EQ(getServiceContext()->getStorageEngine()->getNumDropPendingIdents(),
              numDropPendingIdentsBefore + 5);
}

TEST_F(AbortTest, LeavesIndexBuildsInOtherCollectionsAlone) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    const auto otherNss =
        NamespaceString::createNamespaceString_forTest(_nss.dbName(), "otherCollection");
    auto otherCollectionUUID = createCollection(otherNss);

    startUnresumedPrimaryDrivenIndexBuild();
    startUnresumedPrimaryDrivenIndexBuild(otherNss, otherCollectionUUID);
    ASSERT_EQ(registry().all().size(), 2);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    index_builds::abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] { collection = boost::none; },
        [&] {
            collection.emplace(acquireExclusive());
            return true;
        });

    EXPECT_EQ(numIndexesInProgress(), 0);
    // The build on the collection that is not being dropped is untouched.
    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(otherNss), 1);
}

TEST_F(AbortTest, AbortsUnresumedPrimaryDrivenBuildsOnEveryCollectionInTheDatabase) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    const auto otherNss =
        NamespaceString::createNamespaceString_forTest(_nss.dbName(), "otherCollection");
    auto otherCollectionUUID = createCollection(otherNss);

    startUnresumedPrimaryDrivenIndexBuild();
    startUnresumedPrimaryDrivenIndexBuild(otherNss, otherCollectionUUID);
    ASSERT_EQ(registry().all().size(), 2);

    boost::optional<Lock::DBLock> dbLock;
    dbLock.emplace(opCtx, _nss.dbName(), MODE_X);
    auto commitTimestampGuard = makeCommitTimestampGuard();
    index_builds::abort(
        opCtx,
        _nss.dbName(),
        "database is being dropped",
        [&] { dbLock = boost::none; },
        [&] {
            dbLock.emplace(opCtx, _nss.dbName(), MODE_X);
            return true;
        });

    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    EXPECT_EQ(numIndexesInProgress(otherNss), 0);
    // Both collections and their '_id_' indexes are left behind.
    EXPECT_EQ(numIndexesTotal(_nss), 1);
    EXPECT_EQ(numIndexesTotal(otherNss), 1);
}

TEST_F(AbortTest, AbortsBuildRunByCoordinatorMatchingIndexNames) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    insertDocument();

    // Park the builder thread so that the build is still in progress when we abort it. The abort
    // interrupts the build rather than waiting for the fail point to be lifted.
    FailPointEnableBlock hangAfterInitializingIndexBuild{"hangAfterInitializingIndexBuild"};
    auto future = startCoordinatorRunIndexBuild();

    ASSERT_TRUE(
        IndexBuildsCoordinator::get(opCtx)->hasIndexBuilder(opCtx, _collectionUUID, {"a_1"}));
    ASSERT_EQ(numIndexesInProgress(), 1);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    int unlocks = 0;
    int locks = 0;
    index_builds::abort(
        opCtx,
        _nss,
        _collectionUUID,
        {"a_1"},
        "dropIndexes command",
        [&] {
            ++unlocks;
            collection = boost::none;
        },
        [&] {
            ++locks;
            collection.emplace(acquireExclusive());
            return true;
        });

    // The locks were yielded so that the builder thread could exit, and handed back.
    EXPECT_GT(unlocks, 0);
    EXPECT_EQ(locks, unlocks);
    ASSERT_TRUE(collection.has_value());

    EXPECT_EQ(future.getNoThrow().getStatus(), ErrorCodes::IndexBuildAborted);
    EXPECT_FALSE(
        IndexBuildsCoordinator::get(opCtx)->hasIndexBuilder(opCtx, _collectionUUID, {"a_1"}));
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    EXPECT_EQ(numIndexesTotal(_nss), 1);
}

TEST_F(AbortTest, AbortsBuildRunByCoordinator) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    insertDocument();

    // Park the builder thread so that the build is still in progress when we abort it. The abort
    // interrupts the build rather than waiting for the fail point to be lifted.
    FailPointEnableBlock hangAfterInitializingIndexBuild{"hangAfterInitializingIndexBuild"};
    auto future = startCoordinatorRunIndexBuild();

    ASSERT_TRUE(IndexBuildsCoordinator::get(opCtx)->inProgForCollection(_collectionUUID));
    ASSERT_EQ(numIndexesInProgress(), 1);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    int unlocks = 0;
    int locks = 0;
    index_builds::abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] {
            ++unlocks;
            collection = boost::none;
        },
        [&] {
            ++locks;
            collection.emplace(acquireExclusive());
            return true;
        });

    // The locks were yielded for the abort and handed back.
    EXPECT_GT(unlocks, 0);
    EXPECT_EQ(locks, unlocks);
    ASSERT_TRUE(collection.has_value());

    EXPECT_EQ(future.getNoThrow().getStatus(), ErrorCodes::IndexBuildAborted);
    EXPECT_FALSE(IndexBuildsCoordinator::get(opCtx)->inProgForCollection(_collectionUUID));
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    EXPECT_EQ(numIndexesTotal(_nss), 1);
}

TEST_F(AbortTest, AbortsBothRunningAndUnresumedBuildsOnCollection) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    insertDocument();

    FailPointEnableBlock hangAfterInitializingIndexBuild{"hangAfterInitializingIndexBuild"};
    auto future = startCoordinatorRunIndexBuild();
    startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"b"});
    ASSERT_EQ(registry().all().size(), 2);
    ASSERT_EQ(numIndexesInProgress(), 2);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    int unlocks = 0;
    int locks = 0;
    {
        auto commitTimestampGuard = makeCommitTimestampGuard();
        abort(
            opCtx,
            _nss,
            _collectionUUID,
            "collection is being dropped",
            [&] {
                ++unlocks;
                collection = boost::none;
            },
            [&] {
                ++locks;
                collection.emplace(acquireExclusive());
                return true;
            });
    }

    // The locks were yielded for the running build's abort and again after the inline one.
    EXPECT_GT(unlocks, 1);
    EXPECT_EQ(locks, unlocks);
    ASSERT_TRUE(collection.has_value());

    EXPECT_EQ(future.getNoThrow().getStatus(), ErrorCodes::IndexBuildAborted);
    EXPECT_FALSE(IndexBuildsCoordinator::get(opCtx)->inProgForCollection(_collectionUUID));
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    EXPECT_EQ(numIndexesTotal(_nss), 1);
}

TEST_F(AbortTest, AbortsUnresumedPrimaryDrivenBuildMatchingIndexNames) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild();
    ASSERT_EQ(numIndexesInProgress(), 1);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    abort(
        opCtx,
        _nss,
        _collectionUUID,
        {"a_1"},
        "dropIndexes command",
        [&] { collection = boost::none; },
        [&] {
            collection.emplace(acquireExclusive());
            return true;
        });

    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    EXPECT_EQ(numIndexesTotal(_nss), 1);
}

TEST_F(AbortTest, LeavesUnresumedPrimaryDrivenBuildsOfOtherIndexNamesAlone) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"a", "b"});
    ASSERT_EQ(numIndexesInProgress(), 2);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    auto abortNames = [&](const std::vector<std::string>& indexNames) {
        abort(
            opCtx,
            _nss,
            _collectionUUID,
            indexNames,
            "dropIndexes command",
            [&] { collection = boost::none; },
            [&] {
                collection.emplace(acquireExclusive());
                return true;
            });
    };

    abortNames({"c_1"});
    abortNames({"a_1"});
    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(), 2);

    // Naming the build's whole index set aborts it.
    abortNames({"a_1", "b_1"});
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
}

TEST_F(AbortTest, MatchesUnresumedPrimaryDrivenBuildByExactIndexNameSet) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"a", "b", "c"});
    ASSERT_EQ(numIndexesInProgress(), 3);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    auto abortNames = [&](const std::vector<std::string>& indexNames) {
        abort(
            opCtx,
            _nss,
            _collectionUUID,
            indexNames,
            "dropIndexes command",
            [&] { collection = boost::none; },
            [&] {
                collection.emplace(acquireExclusive());
                return true;
            });
    };

    // A superset of the build's index names does not match it.
    abortNames({"a_1", "b_1", "c_1", "d_1"});
    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(), 3);

    // Neither does a subset, even one that differs by a single name.
    abortNames({"a_1", "b_1"});
    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(), 3);

    // A permutation of exactly the build's index names matches it.
    abortNames({"c_1", "a_1", "b_1"});
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
}

TEST_F(AbortTest, AbortsUnresumedPrimaryDrivenBuildMatchingIndexNamesWhileAnotherBuildRuns) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    insertDocument();

    // Park the builder thread so that its build ('a_1') is still in progress throughout.
    FailPointEnableBlock hangAfterInitializingIndexBuild{"hangAfterInitializingIndexBuild"};
    auto future = startCoordinatorRunIndexBuild();
    startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"b"});
    ASSERT_EQ(registry().all().size(), 2);
    ASSERT_EQ(numIndexesInProgress(), 2);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto unlock = [&] {
        collection = boost::none;
    };
    auto lock = [&] {
        collection.emplace(acquireExclusive());
        return true;
    };

    {
        auto commitTimestampGuard = makeCommitTimestampGuard();
        abort(opCtx, _nss, _collectionUUID, {"b_1"}, "dropIndexes command", unlock, lock);
    }

    // The unresumed build is gone and the running one is untouched.
    EXPECT_TRUE(
        IndexBuildsCoordinator::get(opCtx)->hasIndexBuilder(opCtx, _collectionUUID, {"a_1"}));
    EXPECT_FALSE(future.isReady());
    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(), 1);

    // Aborting the running build's indexes then takes the coordinator branch and joins its thread.
    abort(opCtx, _nss, _collectionUUID, {"a_1"}, "dropIndexes command", unlock, lock);

    EXPECT_EQ(future.getNoThrow().getStatus(), ErrorCodes::IndexBuildAborted);
    EXPECT_TRUE(registry().all().empty());
    EXPECT_EQ(numIndexesInProgress(), 0);
    EXPECT_EQ(numIndexesTotal(_nss), 1);
}

TEST_F(AbortTest, AbortsBuildRunByCoordinatorMatchingIndexNamesWhileAnotherBuildIsUnresumed) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    insertDocument();

    FailPointEnableBlock hangAfterInitializingIndexBuild{"hangAfterInitializingIndexBuild"};
    auto future = startCoordinatorRunIndexBuild();
    auto unresumedBuildUUID = startUnresumedPrimaryDrivenIndexBuild(_nss, _collectionUUID, {"b"});
    ASSERT_EQ(numIndexesInProgress(), 2);

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    abort(
        opCtx,
        _nss,
        _collectionUUID,
        {"a_1"},
        "dropIndexes command",
        [&] { collection = boost::none; },
        [&] {
            collection.emplace(acquireExclusive());
            return true;
        });

    EXPECT_EQ(future.getNoThrow().getStatus(), ErrorCodes::IndexBuildAborted);
    EXPECT_FALSE(
        IndexBuildsCoordinator::get(opCtx)->hasIndexBuilder(opCtx, _collectionUUID, {"a_1"}));

    // The unresumed build is still registered and its index is still being built.
    EXPECT_TRUE(registry().contains(unresumedBuildUUID));
    EXPECT_EQ(registry().all().size(), 1);
    EXPECT_EQ(numIndexesInProgress(), 1);
}

TEST_F(AbortTest, DoesNotYieldLocksWhenThereAreNoIndexBuilds) {
    auto opCtx = operationContext();
    ASSERT_EQ(numIndexesInProgress(), 0);

    auto collection = acquireExclusive();
    int unlocks = 0;
    abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] { ++unlocks; },
        [&] {
            FAIL("locks should not have been yielded");
            return true;
        });

    EXPECT_EQ(unlocks, 0);
}

TEST_F(AbortTest, StopsWhenCallerDeclinesToReacquireLocks) {
    unittest::ServerParameterGuard ffContainerWrites("featureFlagContainerWrites", true);
    unittest::ServerParameterGuard ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto opCtx = operationContext();
    startUnresumedPrimaryDrivenIndexBuild();

    boost::optional<CollectionAcquisition> collection(acquireExclusive());
    auto commitTimestampGuard = makeCommitTimestampGuard();
    int lockAttempts = 0;
    abort(
        opCtx,
        _nss,
        _collectionUUID,
        "collection is being dropped",
        [&] { collection = boost::none; },
        [&] {
            ++lockAttempts;
            return false;
        });

    // The caller gave up after the first yield.
    EXPECT_EQ(lockAttempts, 1);
    EXPECT_FALSE(collection.has_value());
}

}  // namespace
}  // namespace mongo::index_builds
