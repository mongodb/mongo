// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/metadata_consistency_checks/collection_metadata_checks.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_helpers.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace collection_metadata_consistency_checks {

using metadata_consistency_util::makeInconsistency;
using metadata_consistency_util::RSNodeMode;

std::vector<MetadataInconsistencyItem> checkNoMetadataForNonExistentDatabase(
    OperationContext* opCtx,
    const ShardId& shardId,
    RSNodeMode rsMode,
    const stdx::unordered_set<DatabaseName>& dbNamesInGlobalCatalog) {
    metadata_consistency_internal::OptimisticFCVFeatureFlagGuard authoritativeShardsGuard(
        opCtx, feature_flags::gAuthoritativeShardsCRUD);

    std::vector<MetadataInconsistencyItem> inconsistencies;

    // Inspect the durable config.shard.catalog.collections.
    {
        DBDirectClient client(opCtx);
        FindCommandRequest findOp{NamespaceString::kConfigShardCatalogCollectionsNamespace};
        auto cursor = client.find(findOp);
        tassert(
            10078303,
            "Failed to retrieve cursor while reading collection metadata from the shard catalog",
            cursor);

        while (cursor->more()) {
            const auto collDoc = cursor->nextSafe().getOwned();
            CollectionType collectionInShardCatalog;
            try {
                collectionInShardCatalog =
                    CollectionType::parse(collDoc, IDLParserContext("CollectionType"));
            } catch (const DBException& ex) {
                inconsistencies.emplace_back(
                    makeInconsistency(MetadataInconsistencyTypeEnum::kUnparsableShardCatalogEntry,
                                      UnparsableShardCatalogEntryDetails{shardId, ex.reason()}));
                continue;
            }
            const auto& nss = collectionInShardCatalog.getNss();
            if (nss == NamespaceString::kLogicalSessionsNamespace) {
                // The sessions collection is special, as it is a tracked collection that is under
                // the 'config' database (which is an internal database).
                continue;
            }

            const auto& dbName = nss.dbName();
            if (!dbNamesInGlobalCatalog.contains(dbName)) {
                inconsistencies.emplace_back(
                    makeInconsistency(MetadataInconsistencyTypeEnum::
                                          kCollectionMetadataForNonExistingDatabaseInShardCatalog,
                                      CollectionMetadataForNonExistingDatabaseInShardCatalogDetails{
                                          nss, shardId, collectionInShardCatalog.getTimestamp()}));
            }
        }
    }

    // Inspect the in-mem CSS.
    // TODO (SERVER-130947): Also check on delayed secondaries.
    if (rsMode != RSNodeMode::kDelayedSecondary) {
        for (const auto& nss : CollectionShardingState::getCollectionNames(opCtx)) {
            if (nss.dbName().isInternalDb() || nss.isNamespaceAlwaysUntracked() ||
                dbNamesInGlobalCatalog.contains(nss.dbName())) {
                continue;
            }

            const auto& csr = CollectionShardingRuntime::acquireShared(opCtx, nss);
            const auto currentMetadata = csr->getCurrentMetadataIfKnown();
            if (!currentMetadata) {
                // No known metadata.
                continue;
            }

            if (csr->isUnowned()) {
                // This is a valid CSS state for a non-existent database.
                continue;
            }

            if (!currentMetadata->hasRoutingTable()) {
                // Don't consider CSS stating that the collection is UNTRACKED. Even though it is an
                // inconsistency to have UNTRACKED CSS for a non-existent database, it is difficult
                // to make this assertion here, given that we don't have DDL stability and that
                // UNTRACKED metadata is not accompanied by a timestamp we can use to ignore DDLs
                // that committed after the check started. Given that this hypothetical inconsistent
                // state would only be problematic if the shard incorrectly believed to be the
                // dbPrimary shard (and that is already checked by a different check), we skip this
                // check here for simplicity.
                continue;
            }

            // Found a CSS referring to a collection on a non-existent database.
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::
                    kCollectionMetadataForNonExistingDatabaseInShardCatalogCache,
                CollectionMetadataForNonExistingDatabaseInShardCatalogCacheDetails{
                    nss,
                    shardId,
                    ShardVersionFactory::make(currentMetadata->getCollPlacementVersion())}));
        }
    }

    return authoritativeShardsGuard.validateUnchanged() ? inconsistencies
                                                        : std::vector<MetadataInconsistencyItem>{};
}

}  // namespace collection_metadata_consistency_checks
}  // namespace mongo
