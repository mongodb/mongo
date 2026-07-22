// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/commit_database_metadata_locally.h"

#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "TestDB");

class CommitDatabaseMetadataLocallyTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        createTestCollection(operationContext(),
                             NamespaceString::kConfigShardCatalogDatabasesNamespace);

        // Install the real shard DatabaseShardingRuntime so that the commit/drop helpers can drive
        // the in-memory DSR (the default test fixture installs a mock).
        DatabaseShardingStateFactory::clear(getServiceContext());
        DatabaseShardingStateFactory::set(getServiceContext(),
                                          std::make_unique<DatabaseShardingStateFactoryShard>());
    }

    DatabaseType makeDatabaseType() {
        return DatabaseType{kDbName,
                            ShardingState::get(operationContext())->shardId(),
                            DatabaseVersion(UUID::gen(), Timestamp(10, 0))};
    }

    // Outside of a critical section the DatabaseShardingRuntime does not grant write access to the
    // in-memory metadata, so emulate the production clone path which sets up the write-access
    // bypass before driving the commit/drop helpers.
    void commitCreate(const DatabaseType& dbType, bool fromClone = false) {
        BypassDatabaseMetadataAccess bypass(  // NOLINT
            operationContext(),
            BypassDatabaseMetadataAccess::Type::kWriteOnly);
        shard_catalog_commit::commitCreateDatabaseMetadataLocally(
            operationContext(), dbType, fromClone);
    }

    void dropDatabase() {
        BypassDatabaseMetadataAccess bypass(  // NOLINT
            operationContext(),
            BypassDatabaseMetadataAccess::Type::kWriteOnly);
        shard_catalog_commit::commitDropDatabaseMetadataLocally(operationContext(), kDbName);
    }

    long long countLocalDocs(const NamespaceString& nss) {
        DBDirectClient client(operationContext());
        return client.count(nss);
    }

    std::vector<BSONObj> findLocalDocs(const NamespaceString& nss, BSONObj query = BSONObj()) {
        DBDirectClient client(operationContext());
        FindCommandRequest findCmd(nss);
        findCmd.setFilter(std::move(query));
        auto cursor = client.find(std::move(findCmd));
        std::vector<BSONObj> results;
        while (cursor->more()) {
            results.push_back(cursor->next().getOwned());
        }
        return results;
    }

    long long countCommandOplogEntries(std::string_view commandField) {
        const std::string objField = "o." + std::string{commandField};
        return findLocalDocs(NamespaceString::kRsOplogNamespace,
                             BSON("op" << "c" << objField << BSON("$exists" << true)))
            .size();
    }

    BSONObj getDatabaseVersionUpdateCounters() {
        BSONObjBuilder builder;
        ShardingStatistics::get(operationContext()).report(&builder);
        return builder.obj().getObjectField("databaseShardingMetadataStatistics").getOwned();
    }

    boost::optional<DatabaseVersion> getInstalledDbVersion() {
        const auto scopedDsr = DatabaseShardingRuntime::acquireShared(operationContext(), kDbName);
        return scopedDsr->getDbVersion(operationContext());
    }

    repl::MutableOplogEntry makeOplogEntry() {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(NamespaceString::makeCommandNamespace(kDbName));
        oplogEntry.setObject(BSON("test" << 1));
        oplogEntry.setOpTime(OplogSlot());
        oplogEntry.setWallClockTime(operationContext()->fastClockSource().now());
        return oplogEntry;
    }
};

TEST_F(CommitDatabaseMetadataLocallyTest, WriteDatabaseMetadataOplogEntrySetsOpTime) {
    auto oplogEntry = makeOplogEntry();

    shard_catalog_commit::writeDatabaseMetadataOplogEntry(
        operationContext(), oplogEntry, "testDatabaseMetadataOplogEntry");

    ASSERT_FALSE(oplogEntry.getOpTime().isNull());
}

TEST_F(CommitDatabaseMetadataLocallyTest, CommitCreatePersistsMetadataAndInstallsDSR) {
    const auto dbType = makeDatabaseType();

    commitCreate(dbType);

    // The durable database catalog entry is written and the oplog 'c' entry is emitted.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogDatabasesNamespace), 1);
    ASSERT_EQ(countCommandOplogEntries("createDatabaseMetadata"), 1);

    // The commit is counted distinctly and is not treated as a clone.
    auto counters = getDatabaseVersionUpdateCounters();
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataCommits"), 1);
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataClones"), 0);
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataDrops"), 0);

    // The in-memory DSR reflects the committed version.
    auto installed = getInstalledDbVersion();
    ASSERT_TRUE(installed);
    ASSERT_EQ(installed->getTimestamp(), dbType.getVersion().getTimestamp());
}

TEST_F(CommitDatabaseMetadataLocallyTest, CommitCreateFromCloneCountsAsClone) {
    const auto dbType = makeDatabaseType();

    commitCreate(dbType, true /* fromClone */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogDatabasesNamespace), 1);
    ASSERT_EQ(countCommandOplogEntries("createDatabaseMetadata"), 1);

    // The clone-driven create is counted separately from a standalone commit.
    auto counters = getDatabaseVersionUpdateCounters();
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataClones"), 1);
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataCommits"), 0);
}

TEST_F(CommitDatabaseMetadataLocallyTest, CommitCreateClearsUnownedCollectionMetadata) {
    const auto dbType = makeDatabaseType();
    const auto collNss = NamespaceString::createNamespaceString_forTest(kDbName, "coll");
    createTestCollection(operationContext(), collNss);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(operationContext(), collNss);
        scopedCsr->setCollectionMetadata(operationContext(),
                                         CollectionMetadata::UNTRACKED(),
                                         CollectionShardingRuntime::NoRoutingTableAs::kUnowned);
    }

    commitCreate(dbType);

    // A stale, UNOWNED classification left over from before this shard became the DB primary must
    // be cleared so a fresh recovery can classify the collection correctly.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), collNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

TEST_F(CommitDatabaseMetadataLocallyTest,
       CommitCreateFromClonePreservesAlreadyOwnedCollectionMetadata) {
    const auto dbType = makeDatabaseType();
    const auto collNss = NamespaceString::createNamespaceString_forTest(kDbName, "coll");
    createTestCollection(operationContext(), collNss);

    // Creating the collection installs known, UNTRACKED (owned) filtering metadata by default.
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), collNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
        ASSERT_FALSE(scopedCsr->isUnowned());
    }

    commitCreate(dbType, true /* fromClone */);

    // Already-known, owned collection metadata is not cleared just because a create commit (clone
    // or otherwise) ran for the owning database (SERVER-129871).
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), collNss);
    ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
}

TEST_F(CommitDatabaseMetadataLocallyTest, CommitCreateIsIdempotent) {
    const auto dbType = makeDatabaseType();

    commitCreate(dbType);
    commitCreate(dbType);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogDatabasesNamespace), 1);
    ASSERT_EQ(getDatabaseVersionUpdateCounters().getIntField("countLocalDatabaseMetadataCommits"),
              2);
}

TEST_F(CommitDatabaseMetadataLocallyTest, DropDeletesMetadataAndClearsDSR) {
    const auto dbType = makeDatabaseType();

    // First commit the metadata so there is something to drop.
    commitCreate(dbType);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogDatabasesNamespace), 1);
    ASSERT_TRUE(getInstalledDbVersion());

    dropDatabase();

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogDatabasesNamespace), 0);
    ASSERT_EQ(countCommandOplogEntries("dropDatabaseMetadata"), 1);

    // The DSR no longer holds any metadata for the database.
    ASSERT_FALSE(getInstalledDbVersion());

    // The drop is counted distinctly from the commit that seeded the metadata.
    auto counters = getDatabaseVersionUpdateCounters();
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataDrops"), 1);
    ASSERT_EQ(counters.getIntField("countLocalDatabaseMetadataCommits"), 1);
}

TEST_F(CommitDatabaseMetadataLocallyTest, DropIsNoOpOnEmptyCatalog) {
    dropDatabase();

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogDatabasesNamespace), 0);
    // The invalidation oplog entry is still emitted and the drop is still counted.
    ASSERT_EQ(countCommandOplogEntries("dropDatabaseMetadata"), 1);
    ASSERT_EQ(getDatabaseVersionUpdateCounters().getIntField("countLocalDatabaseMetadataDrops"), 1);
}

}  // namespace
}  // namespace mongo
