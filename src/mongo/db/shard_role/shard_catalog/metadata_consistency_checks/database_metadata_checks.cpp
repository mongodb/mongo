// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/metadata_consistency_checks/database_metadata_checks.h"

#include "mongo/base/status_with.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_helpers.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <utility>
#include <vector>

namespace mongo {
namespace database_metadata_consistency_checks {

using metadata_consistency_util::makeInconsistency;
using metadata_consistency_util::RSNodeMode;

namespace {

struct DatabaseMetadataCheckCtx {
    DatabaseName dbName;
    DatabaseType dbInGlobalCatalog;
    ShardId myShardId;
    bool thisShardIsDbPrimary;
    RSNodeMode rsMode;
    bool authoritativeShardsCRUDEnabled;
};

/**
 * Checks the in-memory DSS state:
 *
 * - If the critical section is active, skip the check, as the metadata is in the middle of a
 * commit.
 * - When authoritativeShards is enabled:
 *     - dbPrimary shards must know the correct metadata.
 *     - non-dbPrimary shards must not have metadata installed.
 * - When authoritativeShards is disabled:
 *     - dbPrimary shards are allowed to have unknown or stale information.
 *     - non-dbPrimary shards may have stale information, but must never have wrongfully believe
 *       that they are the dbPrimary shard.
 */
std::vector<MetadataInconsistencyItem> checkShardCatalogCache(
    OperationContext* opCtx, const DatabaseMetadataCheckCtx& checkCtx) {
    std::vector<MetadataInconsistencyItem> inconsistencies;
    // Wait for any in-flight FCV commit callback to finish cleaning stale DSS metadata.
    // TODO: SERVER-98118 Remove this.
    {
        Lock::DBLock fcvDbLock(
            opCtx, NamespaceString::kServerConfigurationNamespace.dbName(), MODE_IS);
        Lock::CollectionLock fcvCollLock(
            opCtx, NamespaceString::kServerConfigurationNamespace, MODE_S);
    }

    const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, checkCtx.dbName);
    if (scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead)) {
        // Database critical section active. Skip check.
        return inconsistencies;
    }

    const auto dssDbPrimary = scopedDsr->getDbPrimaryShard(opCtx);
    const auto dssDbVersion = scopedDsr->getDbVersion(opCtx);
    if (checkCtx.authoritativeShardsCRUDEnabled) {
        if (checkCtx.thisShardIsDbPrimary) {
            // This shard is the dbPrimary for this database. Therefore it must know the correct
            // metadata.
            if (!dssDbVersion) {
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalogCache,
                    MissingDatabaseMetadataInShardCatalogCacheDetails{
                        checkCtx.dbName,
                        checkCtx.myShardId,
                        checkCtx.dbInGlobalCatalog.getVersion()}));
            } else {
                if (dssDbPrimary != checkCtx.dbInGlobalCatalog.getPrimary() ||
                    dssDbVersion != checkCtx.dbInGlobalCatalog.getVersion()) {
                    InconsistentDatabaseVersionInShardCatalogCacheDetails details{
                        checkCtx.dbName,
                        checkCtx.myShardId,
                        checkCtx.dbInGlobalCatalog.getVersion(),
                        *dssDbVersion};
                    details.setPrimaryShardIdInShardCatalogCache(dssDbPrimary);
                    inconsistencies.emplace_back(
                        makeInconsistency(MetadataInconsistencyTypeEnum::
                                              kInconsistentDatabaseVersionInShardCatalogCache,
                                          std::move(details)));
                }
            }
        } else {
            // This shard is not the dbPrimary for this database. Therefore, it must not have DSS
            // metadata for it.
            if (dssDbVersion) {
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalogCache,
                    MisplacedDatabaseMetadataInShardCatalogCacheDetails{
                        checkCtx.dbName,
                        checkCtx.myShardId,
                        dssDbPrimary.value_or(checkCtx.myShardId)}));
            }
        }
    } else {
        // Non authoritative mode. We can only check that if we are not the dbPrimary shard, then
        // we must not incorrectly believe we are. The opposite cannot be asserted.
        const bool cacheThinksThisShardIsPrimary = dssDbPrimary == checkCtx.myShardId;
        if (cacheThinksThisShardIsPrimary && !checkCtx.thisShardIsDbPrimary) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalogCache,
                MisplacedDatabaseMetadataInShardCatalogCacheDetails{
                    checkCtx.dbName, checkCtx.myShardId, *dssDbPrimary}));
        }
    }

    return inconsistencies;
}

StatusWith<boost::optional<DatabaseType>> readDatabaseFromDurableShardCatalog(
    OperationContext* opCtx, const DatabaseName& dbName) {
    DBDirectClient client(opCtx);
    FindCommandRequest findOp{NamespaceString::kConfigShardCatalogDatabasesNamespace};
    findOp.setFilter(BSON(DatabaseType::kDbNameFieldName << DatabaseNameUtil::serialize(
                              dbName, SerializationContext::stateDefault())));
    auto cursor = client.find(std::move(findOp));

    tassert(
        10078301,
        str::stream() << "Failed to retrieve cursor while reading database metadata for database: "
                      << dbName.toStringForErrorMsg(),
        cursor);

    if (!cursor->more()) {
        return boost::none;
    }

    auto databaseDoc = cursor->nextSafe().getOwned();

    tassert(9980501,
            "Found duplicated database metadata in the shard catalog with the same _id value",
            !cursor->more());

    try {
        return DatabaseType::parse(databaseDoc, IDLParserContext("DatabaseType"));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

/**
 * Checks the durable metadata backing the DSS.
 *
 * If the authoritativeShards feature is enabled:
 *  - The dbPrimary shard must know the correct metadata
 *  - The non-dbPrimary shards must not have any metadata.
 */
std::vector<MetadataInconsistencyItem> checkDurableShardCatalog(
    OperationContext* opCtx, const DatabaseMetadataCheckCtx& checkCtx) {
    const auto swDbInShardCatalog = readDatabaseFromDurableShardCatalog(opCtx, checkCtx.dbName);

    // If there's an unparsable entry, always raise an inconsistency.
    if (!swDbInShardCatalog.isOK()) {
        MissingDatabaseMetadataInShardCatalogDetails details{
            checkCtx.dbName, checkCtx.myShardId, checkCtx.dbInGlobalCatalog.getVersion()};
        details.setReason(swDbInShardCatalog.getStatus().reason());
        return {
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog,
                              std::move(details))};
    }

    // If authoritative shards is not fully enabled, we can't assert that all entries are present.
    if (!checkCtx.authoritativeShardsCRUDEnabled) {
        return {};
    }

    const auto& dbInShardCatalog = swDbInShardCatalog.getValue();
    std::vector<MetadataInconsistencyItem> inconsistencies;

    // If this is the dbPrimary shard, the metadata must be present and correct. Otherwise, it must
    // not be present.
    if (checkCtx.thisShardIsDbPrimary) {
        if (!dbInShardCatalog) {
            MissingDatabaseMetadataInShardCatalogDetails details{
                checkCtx.dbName,
                checkCtx.dbInGlobalCatalog.getPrimary(),
                checkCtx.dbInGlobalCatalog.getVersion()};
            return {makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog,
                std::move(details))};
        }

        const auto dbVersionInShardCatalog = dbInShardCatalog->getVersion();
        if (dbVersionInShardCatalog != checkCtx.dbInGlobalCatalog.getVersion()) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog,
                InconsistentDatabaseVersionInShardCatalogDetails{
                    checkCtx.dbName,
                    checkCtx.myShardId,
                    checkCtx.dbInGlobalCatalog.getVersion(),
                    dbVersionInShardCatalog}));
        }

        if (dbInShardCatalog->getPrimary() != checkCtx.dbInGlobalCatalog.getPrimary()) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog,
                MisplacedDatabaseMetadataInShardCatalogDetails{
                    checkCtx.dbName, checkCtx.myShardId, dbInShardCatalog->getPrimary()}));
        }
    } else {
        if (dbInShardCatalog) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalog,
                MisplacedDatabaseMetadataInShardCatalogDetails{
                    checkCtx.dbName, checkCtx.myShardId, dbInShardCatalog->getPrimary()}));
        }
    }

    return inconsistencies;
}

}  // namespace

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistency(
    OperationContext* opCtx,
    const DatabaseType& dbInGlobalCatalog,
    const ShardId& shardId,
    RSNodeMode rsMode) {
    metadata_consistency_internal::OptimisticFCVFeatureFlagGuard authoritativeShardsGuard(
        opCtx, feature_flags::gAuthoritativeShardsCRUD);

    const DatabaseMetadataCheckCtx checkCtx{
        .dbName = dbInGlobalCatalog.getDbName(),
        .dbInGlobalCatalog = dbInGlobalCatalog,
        .myShardId = shardId,
        .thisShardIsDbPrimary = dbInGlobalCatalog.getPrimary() == shardId,
        .rsMode = rsMode,
        .authoritativeShardsCRUDEnabled = authoritativeShardsGuard.wasEnabled()};

    if (checkCtx.dbName.isInternalDb()) {
        return {};
    }

    // Check the durable shard catalog state.
    std::vector<MetadataInconsistencyItem> inconsistencies =
        checkDurableShardCatalog(opCtx, checkCtx);

    // Check the in-memory DSS state.
    //
    // There is currently no way to retrieve a DSR from a given timestamp, so we skip this check on
    // delayed secondaries.
    // TODO (SERVER-130947): maybe you can.
    if (checkCtx.rsMode != RSNodeMode::kDelayedSecondary) {
        auto cacheInconsistencies = checkShardCatalogCache(opCtx, checkCtx);

        inconsistencies.insert(inconsistencies.end(),
                               std::make_move_iterator(cacheInconsistencies.begin()),
                               std::make_move_iterator(cacheInconsistencies.end()));
    }

    // Return the inconsistencies, only if the feature flag enablement state has not changed during
    // the checks.
    return authoritativeShardsGuard.validateUnchanged() ? inconsistencies
                                                        : std::vector<MetadataInconsistencyItem>{};
}

}  // namespace database_metadata_consistency_checks
}  // namespace mongo
