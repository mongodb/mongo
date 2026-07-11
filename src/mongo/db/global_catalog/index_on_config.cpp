// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/index_on_config.h"

#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

std::vector<IndexSpec_ForCatalog> getChunkCollectionIndexSpecs() {
    return {
        {BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1), true /* unique */},
        {BSON(ChunkType::collectionUUID() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
         true /* unique */},
        {BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1), true /* unique */},
        {BSON(ChunkType::collectionUUID()
              << 1 << ChunkType::shard() << 1 << ChunkType::onCurrentShardSince() << 1),
         false /* unique */},
    };
}

Status ensureCollectionIndexes(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const std::vector<IndexSpec_ForCatalog>& specs) {
    for (const auto& spec : specs) {
        Status result = sharding_util::createIndexOnCollection(opCtx, nss, spec.keys, spec.unique);
        if (!result.isOK()) {
            return result.withContext(str::stream()
                                      << "couldn't create index " << spec.keys.toString() << " on "
                                      << nss.toStringForErrorMsg());
        }
    }
    return Status::OK();
}

Status createIndexOnConfigCollection(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     const BSONObj& keys,
                                     bool unique) {
    invariant(ns.isConfigDB() || ns.isAdminDB());

    return sharding_util::createIndexOnCollection(opCtx, ns, keys, unique);
}

}  // namespace mongo
