// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"

#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"

namespace mongo {

void ChunkOperationShardingCoordinatorMixin::_checkSetAllowChunkOperations(
    OperationContext* opCtx, const NamespaceString& nss) {
    const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection is undergoing changes so the chunk operation is not allowed",
            scopedCsr->allowChunkOperations());

    const auto optMetadata = scopedCsr->getCurrentMetadataIfKnown();

    uassert(
        StaleConfigInfo(nss,
                        ShardVersionFactory::make(ChunkVersion::IGNORED()) /* receivedVersion */,
                        boost::none /* wantedVersion */,
                        ShardingState::get(opCtx)->shardId()),
        str::stream() << "Collection " << nss.toStringForErrorMsg() << " needs to be recovered",
        optMetadata);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection is undergoing changes so the chunk operation is not allowed",
            optMetadata->allowMigrations());
}

}  // namespace mongo
