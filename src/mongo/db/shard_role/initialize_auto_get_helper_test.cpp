// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/initialize_auto_get_helper.h"

#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/s/session_catalog_router.h"

namespace mongo {
namespace {

class InitializeAutoGetHelperTest : public ShardServerTestFixtureWithCatalogCacheMock {
protected:
    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        Grid::get(operationContext())->setShardingInitialized();
    }

    const ShardId otherShard{"shardB"};
};

TEST_F(InitializeAutoGetHelperTest, TrackedUnshardedSecondaryCollections) {
    auto opCtx = operationContext();

    // nssA and nssC are colocated. nssB is on a different shard.
    const DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const NamespaceString nssA = NamespaceString::createNamespaceString_forTest(dbName1, "collA");
    const NamespaceString nssB = NamespaceString::createNamespaceString_forTest(dbName1, "collB");
    const NamespaceString nssC = NamespaceString::createNamespaceString_forTest(dbName1, "collC");

    const auto db1PrimaryShard = kMyShardName;
    const auto db1Version = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto criNssA = CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
        nssA, db1PrimaryShard, db1Version, kMyShardName);
    const auto criNssB = CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
        nssB, db1PrimaryShard, db1Version, otherShard);
    const auto criNssC = CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
        nssC, db1PrimaryShard, db1Version, kMyShardName);

    getCatalogCacheMock()->setCollectionReturnValue(nssA, criNssA);
    getCatalogCacheMock()->setCollectionReturnValue(nssB, criNssB);
    getCatalogCacheMock()->setCollectionReturnValue(nssC, criNssC);

    ScopedSetShardRole shardRoleA(opCtx, nssA, criNssA.getShardVersion(kMyShardName), boost::none);

    // test with secondaryNss={nssB}.
    bool anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(criNssA.getShardVersion(kMyShardName), oss.getShardVersion(nssA));
        ASSERT_EQ(criNssB.getShardVersion(kMyShardName), oss.getShardVersion(nssB));
        ASSERT_EQ(boost::none, oss.getDbVersion(dbName1));
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);

    // test with secondaryNss={nssC}.
    anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssC}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(criNssA.getShardVersion(kMyShardName), oss.getShardVersion(nssA));
        ASSERT_EQ(criNssC.getShardVersion(kMyShardName), oss.getShardVersion(nssC));
        ASSERT_EQ(boost::none, oss.getDbVersion(dbName1));
    });
    ASSERT_EQ(false, anySecondaryCollectionIsNotLocal);

    // test with secondaryNss={nssB, nssC}.
    anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB, nssC}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(criNssA.getShardVersion(kMyShardName), oss.getShardVersion(nssA));
        ASSERT_EQ(criNssB.getShardVersion(kMyShardName), oss.getShardVersion(nssB));
        ASSERT_EQ(criNssC.getShardVersion(kMyShardName), oss.getShardVersion(nssC));
        ASSERT_EQ(boost::none, oss.getDbVersion(dbName1));
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);

    // test with secondaryNss={nssA, nssC}.
    anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssA, nssC}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(criNssA.getShardVersion(kMyShardName), oss.getShardVersion(nssA));
        ASSERT_EQ(criNssC.getShardVersion(kMyShardName), oss.getShardVersion(nssC));
        ASSERT_EQ(boost::none, oss.getDbVersion(dbName1));
    });
    ASSERT_EQ(false, anySecondaryCollectionIsNotLocal);
}

TEST_F(InitializeAutoGetHelperTest, UntrackedSecondaryCollections) {
    auto opCtx = operationContext();

    // db1 is on shardA. db2 is on shardB.
    const DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const DatabaseName dbName2 = DatabaseName::createDatabaseName_forTest(boost::none, "db2");

    // Untracked collections. nssA and nssB are in db1. nssC is in db2.
    const NamespaceString nssA = NamespaceString::createNamespaceString_forTest(dbName1, "collA");
    const NamespaceString nssB = NamespaceString::createNamespaceString_forTest(dbName1, "collB");
    const NamespaceString nssC = NamespaceString::createNamespaceString_forTest(dbName1, "collC");

    const auto db1PrimaryShard = kMyShardName;
    const auto db1Version = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto db2PrimaryShard = otherShard;
    const auto db2Version = DatabaseVersion(UUID::gen(), Timestamp(2, 0));

    const auto criNssA =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nssA, db1PrimaryShard, db1Version);
    const auto criNssB =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nssB, db1PrimaryShard, db1Version);
    const auto criNssC =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nssC, db2PrimaryShard, db2Version);

    getCatalogCacheMock()->setCollectionReturnValue(nssA, criNssA);
    getCatalogCacheMock()->setCollectionReturnValue(nssB, criNssB);
    getCatalogCacheMock()->setCollectionReturnValue(nssC, criNssC);

    ScopedSetShardRole shardRoleA(opCtx, nssA, ShardVersion::UNTRACKED(), boost::none);

    // test with secondaryNss={nssB}.
    bool anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssB));
        // Expect DatabaseVersion for dbName1 to be set since the cache believes this shard to be
        // its dbPrimary shard.
        ASSERT_EQ(db1Version, oss.getDbVersion(dbName1));
    });
    ASSERT_EQ(false, anySecondaryCollectionIsNotLocal);

    // test with secondaryNss={nssC}.
    anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssC}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssC));
        // Do not expect DatabaseVersion for dbName2 to be set since the cache believes this shard
        // to not be its dbPrimary shard.
        ASSERT_EQ(boost::none, oss.getDbVersion(dbName2));
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);

    // test with secondaryNss={nssB, nssC}.
    anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB, nssC}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssB));
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssC));

        // Expect DatabaseVersion for dbName1 to be set since the cache believes this shard to be
        // its dbPrimary shard.
        ASSERT_EQ(db1Version, oss.getDbVersion(dbName1));
        // Do not expect DatabaseVersion for dbName2 to be set since the cache believes this shard
        // to not be its dbPrimary shard.
        ASSERT_EQ(boost::none, oss.getDbVersion(dbName2));
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);
}

TEST_F(InitializeAutoGetHelperTest, ShardedSecondaryCollections) {
    auto opCtx = operationContext();

    const DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const NamespaceString nssA = NamespaceString::createNamespaceString_forTest(dbName1, "collA");
    const NamespaceString nssB = NamespaceString::createNamespaceString_forTest(dbName1, "collB");

    const auto db1PrimaryShard = kMyShardName;
    const auto db1Version = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto criNssB = CatalogCacheMock::makeCollectionRoutingInfoSharded(
        nssB,
        db1PrimaryShard,
        db1Version,
        BSON("skey" << 1),
        {{ChunkRange(BSON("skey" << MINKEY), BSON("skey" << 0)), kMyShardName},
         {ChunkRange(BSON("skey" << 0), BSON("skey" << MAXKEY)), otherShard}});

    getCatalogCacheMock()->setCollectionReturnValue(nssB, criNssB);

    ScopedSetShardRole shardRoleA(opCtx, nssA, ShardVersion::UNTRACKED(), db1Version);

    // test with secondaryNss={nssB}.
    bool anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(db1Version, oss.getDbVersion(dbName1));
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssA));
        ASSERT_EQ(criNssB.getShardVersion(kMyShardName), oss.getShardVersion(nssB));
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);
}

TEST_F(InitializeAutoGetHelperTest, NoSecondaryCollections) {
    auto opCtx = operationContext();

    const DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const auto db1Version = DatabaseVersion(UUID::gen(), Timestamp(1, 0));
    const NamespaceString nssA = NamespaceString::createNamespaceString_forTest(dbName1, "collA");


    ScopedSetShardRole shardRoleA(opCtx, nssA, ShardVersion::UNTRACKED(), db1Version);
    bool anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);
        ASSERT_EQ(db1Version, oss.getDbVersion(dbName1));
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssA));
    });
    ASSERT_EQ(false, anySecondaryCollectionIsNotLocal);
}

TEST_F(InitializeAutoGetHelperTest, MixedTypeSecondaryCollections) {
    auto opCtx = operationContext();

    // nssA is tracked-unsplittable on this shard. nssB is sharded. nssC is untracked on this shard.
    const DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "db1");

    const NamespaceString nssA = NamespaceString::createNamespaceString_forTest(dbName1, "collA");
    const NamespaceString nssB = NamespaceString::createNamespaceString_forTest(dbName1, "collB");
    const NamespaceString nssC = NamespaceString::createNamespaceString_forTest(dbName1, "collC");

    const auto db1PrimaryShard = kMyShardName;
    const auto db1Version = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto criNssA = CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
        nssA, db1PrimaryShard, db1Version, kMyShardName);

    const auto criNssB = CatalogCacheMock::makeCollectionRoutingInfoSharded(
        nssB,
        db1PrimaryShard,
        db1Version,
        BSON("skey" << 1),
        {{ChunkRange(BSON("skey" << MINKEY), BSON("skey" << MAXKEY)), otherShard}});

    const auto criNssC =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nssC, db1PrimaryShard, db1Version);

    getCatalogCacheMock()->setCollectionReturnValue(nssB, criNssB);
    getCatalogCacheMock()->setCollectionReturnValue(nssC, criNssC);

    ScopedSetShardRole shardRoleA(opCtx, nssA, criNssA.getShardVersion(kMyShardName), boost::none);

    // test with secondaryNss={nssB, nssC}.
    bool anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB, nssC}, [&]() {
        auto& oss = OperationShardingState::get(opCtx);

        ASSERT_EQ(criNssA.getShardVersion(kMyShardName), oss.getShardVersion(nssA));

        ASSERT_EQ(criNssB.getShardVersion(kMyShardName), oss.getShardVersion(nssB));

        ASSERT_EQ(db1Version, oss.getDbVersion(dbName1));
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssC));
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);
}

// Check placementConflict timestamp is set correctly on shardVersion and dbVersion.
TEST_F(InitializeAutoGetHelperTest, InTransactionRcLocal) {
    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(10);
    opCtx->setActiveTransactionParticipant();
    opCtx->setInMultiDocumentTransaction();
    const auto afterClusterTime = LogicalTime(Timestamp(1000, 0));
    repl::ReadConcernArgs::get(opCtx) =
        repl::ReadConcernArgs(afterClusterTime, repl::ReadConcernLevel::kLocalReadConcern);
    RouterOperationContextSession rocs(opCtx);

    // nssA is tracked-unsplittable on this shard. nssB is sharded. nssC is untracked on this shard.
    const DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "db1");

    const NamespaceString nssA = NamespaceString::createNamespaceString_forTest(dbName1, "collA");
    const NamespaceString nssB = NamespaceString::createNamespaceString_forTest(dbName1, "collB");
    const NamespaceString nssC = NamespaceString::createNamespaceString_forTest(dbName1, "collC");

    const auto db1PrimaryShard = kMyShardName;
    const auto db1Version = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto criNssA = CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
        nssA, db1PrimaryShard, db1Version, kMyShardName);

    const auto criNssB = CatalogCacheMock::makeCollectionRoutingInfoSharded(
        nssB,
        db1PrimaryShard,
        db1Version,
        BSON("skey" << 1),
        {{ChunkRange(BSON("skey" << MINKEY), BSON("skey" << MAXKEY)), otherShard}});

    const auto criNssC =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nssC, db1PrimaryShard, db1Version);

    getCatalogCacheMock()->setCollectionReturnValue(nssB, criNssB);
    getCatalogCacheMock()->setCollectionReturnValue(nssC, criNssC);

    ScopedSetShardRole shardRoleA(opCtx, nssA, criNssA.getShardVersion(kMyShardName), boost::none);

    // test with secondaryNss={nssB, nssC}.
    bool anySecondaryCollectionIsNotLocal = initializeAutoGet(opCtx, nssA, {nssB, nssC}, [&]() {
        auto txnRouter = TransactionRouter::get(opCtx);
        ASSERT(txnRouter);

        auto& oss = OperationShardingState::get(opCtx);

        ASSERT_EQ(criNssA.getShardVersion(kMyShardName), oss.getShardVersion(nssA));

        ASSERT_EQ(criNssB.getShardVersion(kMyShardName), oss.getShardVersion(nssB));
        ASSERT_EQ(afterClusterTime, oss.getShardVersion(nssB)->placementConflictTime_DEPRECATED());

        ASSERT_EQ(db1Version, oss.getDbVersion(dbName1));
        ASSERT_EQ(afterClusterTime,
                  oss.getDbVersion(dbName1)->getPlacementConflictTime_DEPRECATED());
        ASSERT_EQ(ShardVersion::UNTRACKED(), oss.getShardVersion(nssC));
        ASSERT_EQ(afterClusterTime, oss.getShardVersion(nssC)->placementConflictTime_DEPRECATED());
    });
    ASSERT_EQ(true, anySecondaryCollectionIsNotLocal);
}

// --- resolveShardRoleVersions ------------------------------------------------------------------

TEST_F(InitializeAutoGetHelperTest, ResolveShardRoleVersionsTracked) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto cri = CatalogCacheMock::makeCollectionRoutingInfoSharded(
        nss,
        kMyShardName,
        dbVersion,
        BSON("skey" << 1),
        {{ChunkRange(BSON("skey" << MINKEY), BSON("skey" << MAXKEY)), kMyShardName}});

    auto [shardVersion, resolvedDbVersion] = resolveShardRoleVersions(
        operationContext(), cri, kMyShardName, boost::none /* placementConflictTime */);

    // Tracked collection: shardVersion is this shard's, dbVersion is unset.
    ASSERT_EQ(cri.getShardVersion(kMyShardName), shardVersion);
    ASSERT_EQ(boost::none, resolvedDbVersion);
}

TEST_F(InitializeAutoGetHelperTest, ResolveShardRoleVersionsUntrackedOnPrimary) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    const auto cri =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nss, kMyShardName, dbVersion);

    auto [shardVersion, resolvedDbVersion] = resolveShardRoleVersions(
        operationContext(), cri, kMyShardName, boost::none /* placementConflictTime */);

    // Untracked collection on its dbPrimary: UNTRACKED sentinel + the DatabaseVersion attached.
    ASSERT_EQ(ShardVersion::UNTRACKED(), shardVersion);
    ASSERT_EQ(dbVersion, resolvedDbVersion);
}

TEST_F(InitializeAutoGetHelperTest, ResolveShardRoleVersionsUntrackedOnNonPrimary) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 0));

    // dbPrimary is otherShard, so this shard must not attach the DatabaseVersion.
    const auto cri =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nss, otherShard, dbVersion);

    auto [shardVersion, resolvedDbVersion] = resolveShardRoleVersions(
        operationContext(), cri, kMyShardName, boost::none /* placementConflictTime */);

    ASSERT_EQ(ShardVersion::UNTRACKED(), shardVersion);
    ASSERT_EQ(boost::none, resolvedDbVersion);
}

TEST_F(InitializeAutoGetHelperTest, ResolveShardRoleVersionsStampsPlacementConflictTime) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db1");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");
    const auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 0));
    const auto placementConflictTime = LogicalTime(Timestamp(1000, 0));

    // Untracked on primary so both shardVersion and dbVersion are produced and stamped.
    const auto cri =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nss, kMyShardName, dbVersion);

    auto [shardVersion, resolvedDbVersion] =
        resolveShardRoleVersions(operationContext(), cri, kMyShardName, placementConflictTime);

    ASSERT_EQ(placementConflictTime, shardVersion.placementConflictTime_DEPRECATED());
    ASSERT(resolvedDbVersion);
    ASSERT_EQ(placementConflictTime, resolvedDbVersion->getPlacementConflictTime_DEPRECATED());
}

}  // namespace
}  // namespace mongo
