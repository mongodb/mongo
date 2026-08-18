// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/metadata_consistency_checks/database_metadata_checks.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_mock.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <vector>

namespace mongo {
namespace {

const ShardId kShard0{"shard0"};
const ShardId kShard1{"shard1"};

void assertOneInconsistencyFound(const MetadataInconsistencyTypeEnum& type,
                                 const std::vector<MetadataInconsistencyItem>& inconsistencies) {
    ASSERT_EQ(1, inconsistencies.size());
    ASSERT_EQ(type, inconsistencies[0].getType());
}

class DatabaseMetadataChecksTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        // TODO(SERVER-98118): for `OptimisticFCVFeatureFlagGuard`, remove once 9.0 is last LTS
        // (Generic FCV reference): used for testing
        FeatureCompatibilityVersion::setIfCleanStartup(
            operationContext(),
            repl::StorageInterface::get(getServiceContext()),
            multiversion::GenericFCV::kLatest);
    }

    DatabaseType makeDatabaseMetadata(const ShardId& primary,
                                      Timestamp timestamp = Timestamp{1, 0}) const {
        return {_dbName, primary, {_dbUuid, timestamp}};
    }

    void setInMemoryDatabaseMetadata(const DatabaseType& metadata) {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->setDbMetadata(operationContext(), metadata);
    }

    void clearInMemoryDatabaseMetadata() {
        auto scopedDsr = DatabaseShardingStateMock::acquire(operationContext(), _dbName);
        scopedDsr->clearDbMetadata(operationContext());
    }

    void insertDurableDatabaseMetadata(const DatabaseType& metadata) {
        DBDirectClient client(operationContext());
        client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace, metadata.toBSON());
    }

    void clearDurableDatabaseMetadata() {
        DBDirectClient client(operationContext());
        client.remove(NamespaceString::kConfigShardCatalogDatabasesNamespace, BSONObj{});
    }

    std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistency(
        const DatabaseType& dbInGlobalCatalog) {
        return database_metadata_consistency_checks::checkDatabaseMetadataConsistency(
            operationContext(), dbInGlobalCatalog, kMyShardName);
    }

    std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistencyNonAuthoritative(
        const DatabaseType& dbInGlobalCatalog) {
        unittest::ServerParameterGuard featureFlagController("featureFlagAuthoritativeShardsCRUD",
                                                             false);
        return checkDatabaseMetadataConsistency(dbInGlobalCatalog);
    }

    const DatabaseName _dbName =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl").dbName();
    const UUID _dbUuid = UUID::gen();
};

// -----------------------------------------------------------------------------------------------
// Tests for in-memory DSS metadata checks
// -----------------------------------------------------------------------------------------------

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryHasCorrectMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    insertDurableDatabaseMetadata(dbInGlobalCatalog);
    setInMemoryDatabaseMetadata(dbInGlobalCatalog);

    ASSERT_TRUE(checkDatabaseMetadataConsistency(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryIsMissingMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    insertDurableDatabaseMetadata(dbInGlobalCatalog);
    clearInMemoryDatabaseMetadata();

    const auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalogCache,
        inconsistencies);
}

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryHasIncorrectMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    insertDurableDatabaseMetadata(dbInGlobalCatalog);

    // Incorrect 'dbVersion'
    setInMemoryDatabaseMetadata(makeDatabaseMetadata(kMyShardName, Timestamp{2, 0}));
    auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalogCache,
        inconsistencies);

    // Incorrect 'primary'.
    setInMemoryDatabaseMetadata(makeDatabaseMetadata(kShard1));
    inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalogCache,
        inconsistencies);
}

TEST_F(DatabaseMetadataChecksTest, WhenNonDbPrimaryHasNoMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    clearInMemoryDatabaseMetadata();

    ASSERT_TRUE(checkDatabaseMetadataConsistency(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest, WhenNonDbPrimaryHasMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    setInMemoryDatabaseMetadata(dbInGlobalCatalog);

    const auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalogCache,
        inconsistencies);
}

// -----------------------------------------------------------------------------------------------
// Tests for durable metadata checks
// -----------------------------------------------------------------------------------------------

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryHasCorrectDurableMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    setInMemoryDatabaseMetadata(dbInGlobalCatalog);
    insertDurableDatabaseMetadata(dbInGlobalCatalog);

    ASSERT_TRUE(checkDatabaseMetadataConsistency(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryIsMissingDurableMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    setInMemoryDatabaseMetadata(dbInGlobalCatalog);

    const auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog, inconsistencies);
}

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryHasIncorrectDurableMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    setInMemoryDatabaseMetadata(dbInGlobalCatalog);

    // Incorrect 'dbVersion'
    insertDurableDatabaseMetadata(makeDatabaseMetadata(kMyShardName, Timestamp{2, 0}));
    auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog, inconsistencies);

    // Incorrect 'primary'
    clearDurableDatabaseMetadata();
    insertDurableDatabaseMetadata(makeDatabaseMetadata(kShard1));
    inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);
    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog, inconsistencies);
}

TEST_F(DatabaseMetadataChecksTest, WhenNonDbPrimaryIsMissingDurableMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    clearInMemoryDatabaseMetadata();

    ASSERT_TRUE(checkDatabaseMetadataConsistency(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest, WhenNonDbPrimaryHasDurableMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    clearInMemoryDatabaseMetadata();
    insertDurableDatabaseMetadata(dbInGlobalCatalog);

    const auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalog, inconsistencies);
}

TEST_F(DatabaseMetadataChecksTest, WhenDbPrimaryHasMalformedDurableMetadata_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    setInMemoryDatabaseMetadata(dbInGlobalCatalog);

    DBDirectClient client(operationContext());
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  BSON(DatabaseType::kDbNameFieldName << _dbName.toString_forTest()));

    const auto inconsistencies = checkDatabaseMetadataConsistency(dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog, inconsistencies);
    ASSERT_EQ(inconsistencies[0].getDetails().getStringField("reason"),
              "BSON field 'DatabaseType.primary' is missing but a required field");
}

// -----------------------------------------------------------------------------------------------
// Tests for non-authoritative in-memory DSS metadata checks
// -----------------------------------------------------------------------------------------------

TEST_F(DatabaseMetadataChecksTest, WhenNonAuthoritativeDbPrimaryHasNoMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);
    clearInMemoryDatabaseMetadata();

    ASSERT_TRUE(checkDatabaseMetadataConsistencyNonAuthoritative(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest, WhenNonAuthoritativeDbPrimaryHasMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kMyShardName);

    setInMemoryDatabaseMetadata(dbInGlobalCatalog);
    ASSERT_TRUE(checkDatabaseMetadataConsistencyNonAuthoritative(dbInGlobalCatalog).empty());

    setInMemoryDatabaseMetadata(makeDatabaseMetadata(kShard1, Timestamp{2, 0}));
    ASSERT_TRUE(checkDatabaseMetadataConsistencyNonAuthoritative(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest,
       WhenNonAuthoritativeNonDbPrimaryHasNoMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    clearInMemoryDatabaseMetadata();

    ASSERT_TRUE(checkDatabaseMetadataConsistencyNonAuthoritative(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest,
       WhenNonAuthoritativeNonDbPrimaryHasStaleMetadata_ThenNoInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    setInMemoryDatabaseMetadata(makeDatabaseMetadata(kShard0, Timestamp{2, 0}));

    ASSERT_TRUE(checkDatabaseMetadataConsistencyNonAuthoritative(dbInGlobalCatalog).empty());
}

TEST_F(DatabaseMetadataChecksTest,
       WhenNonAuthoritativeNonDbPrimaryClaimsToBePrimary_ThenInconsistency) {
    const auto dbInGlobalCatalog = makeDatabaseMetadata(kShard1);
    setInMemoryDatabaseMetadata(makeDatabaseMetadata(kMyShardName));

    const auto inconsistencies =
        checkDatabaseMetadataConsistencyNonAuthoritative(dbInGlobalCatalog);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalogCache,
        inconsistencies);
}

TEST_F(DatabaseMetadataChecksTest, CheckDatabaseMetadataConsistency_CriticalSection) {
    // The DSS metadata may be transiently inconsistent while the critical section is active.
    Timestamp dbTimestamp{1, 0};
    DatabaseVersion dbVersion{_dbUuid, dbTimestamp};
    DatabaseType dbInGlobalCatalog{_dbName, kMyShardName, dbVersion};
    DBDirectClient client(operationContext());
    client.insert(NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  dbInGlobalCatalog.toBSON());

    // Mock that the critical section is acquired in the DSS.
    {
        AutoGetDb autoDb(operationContext(), _dbName, MODE_IX);
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(operationContext(), _dbName);
        scopedDsr->enterCriticalSectionCatchUpPhase(operationContext(), BSON("reason" << "test"));
        scopedDsr->enterCriticalSectionCommitPhase(operationContext(), BSON("reason" << "test"));
    }

    // Skip the DSS check while the critical section is active because the metadata may be
    // transient.
    const auto inconsistencies =
        database_metadata_consistency_checks::checkDatabaseMetadataConsistency(
            operationContext(), dbInGlobalCatalog, kMyShardName);
    ASSERT_TRUE(inconsistencies.empty());

    auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(operationContext(), _dbName);
    scopedDsr->exitCriticalSectionNoChecks(operationContext());
}

}  // namespace
}  // namespace mongo
