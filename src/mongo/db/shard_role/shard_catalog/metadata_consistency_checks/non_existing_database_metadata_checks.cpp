// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/metadata_consistency_checks/non_existing_database_metadata_checks.h"

#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/shard_role/shard_catalog/metadata_consistency_checks/collection_metadata_checks.h"
#include "mongo/db/shard_role/shard_catalog/metadata_consistency_checks/database_metadata_checks.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/stdx/unordered_set.h"

#include <vector>

namespace mongo {
namespace non_existing_database_metadata_consistency_checks {

using metadata_consistency_util::RSNodeMode;

namespace {

/**
 * Returns the set of database names known to the global catalog.
 */
stdx::unordered_set<DatabaseName> getDbNamesInGlobalCatalog(OperationContext* opCtx) {
    const auto dbsInGlobalCatalog =
        Grid::get(opCtx)->catalogClient()->getAllDBs(opCtx, repl::ReadConcernArgs::kMajority);
    stdx::unordered_set<DatabaseName> dbNamesInGlobalCatalog;
    for (const auto& db : dbsInGlobalCatalog) {
        dbNamesInGlobalCatalog.insert(db.getDbName());
    }
    return dbNamesInGlobalCatalog;
}

}  // namespace

std::vector<MetadataInconsistencyItem> checkNonExistingDatabaseMetadataConsistency(
    OperationContext* opCtx, const ShardId& shardId, RSNodeMode rsMode) {
    // Read the list of existing databases from the configsvr.
    const auto dbNamesInGlobalCatalog = getDbNamesInGlobalCatalog(opCtx);

    // See if there is any database metadata referring to a non-existent database.
    auto candidates = database_metadata_consistency_checks::checkNoMetadataForNonExistentDatabase(
        opCtx, shardId, rsMode, dbNamesInGlobalCatalog);

    // See if there is any collection metadata referring to a collection in a non-existent database.
    auto collectionCandidates =
        collection_metadata_consistency_checks::checkNoMetadataForNonExistentDatabase(
            opCtx, shardId, rsMode, dbNamesInGlobalCatalog);
    candidates.insert(candidates.end(),
                      std::make_move_iterator(collectionCandidates.begin()),
                      std::make_move_iterator(collectionCandidates.end()));

    if (candidates.empty()) {
        return {};
    }

    // We have found candidate inconsistencies. Because we are not holding DDL locks for the
    // non-existing databases, we need to verify these are not false positive due to a concurrent
    // DDL having interleaved with the check. To do so, fetch the list of existing databases from
    // the configsvr again and re-run the check. We report only the inconsistencies that were
    // detected with the exact same details as the initial run.
    const auto dbNamesInGlobalCatalogPostCheck = getDbNamesInGlobalCatalog(opCtx);

    auto recheckInconsistencies =
        database_metadata_consistency_checks::checkNoMetadataForNonExistentDatabase(
            opCtx, shardId, rsMode, dbNamesInGlobalCatalogPostCheck);
    auto recheckCollInconsistencies =
        collection_metadata_consistency_checks::checkNoMetadataForNonExistentDatabase(
            opCtx, shardId, rsMode, dbNamesInGlobalCatalogPostCheck);
    recheckInconsistencies.insert(recheckInconsistencies.end(),
                                  std::make_move_iterator(recheckCollInconsistencies.begin()),
                                  std::make_move_iterator(recheckCollInconsistencies.end()));

    // Keep only inconsistencies that still appear identically after the re-check. Comparing both
    // the type and the details BSON ensures that the local state has not changed (e.g. a version
    // bump from a concurrent DDL would produce a different details object and the original
    // inconsistency would be discarded as stale).
    std::vector<MetadataInconsistencyItem> inconsistencies;
    for (auto& candidate : candidates) {
        const auto matches =
            std::any_of(recheckInconsistencies.begin(),
                        recheckInconsistencies.end(),
                        [&](const MetadataInconsistencyItem& reCheck) {
                            return candidate.getType() == reCheck.getType() &&
                                candidate.getDetails().woCompare(reCheck.getDetails()) == 0;
                        });
        if (matches) {
            inconsistencies.push_back(std::move(candidate));
        }
    }

    return inconsistencies;
}

}  // namespace non_existing_database_metadata_consistency_checks
}  // namespace mongo
