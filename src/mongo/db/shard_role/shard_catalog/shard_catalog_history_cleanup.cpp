/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/shard_role/shard_catalog/shard_catalog_history_cleanup.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/topology/sharding_state.h"

namespace mongo::shard_catalog_helper {

StorageEngine::TimestampMonitor::TimestampListener kShardCatalogHistoryCleanupTimestampListener(
    [](OperationContext* opCtx, const StorageEngine::TimestampMonitor::Timestamps& timestamp) {
        auto oldest = timestamp.oldest;
        auto* service = opCtx->getServiceContext();
        auto const shardingState = ShardingState::get(service);
        if (!shardingState->enabled()) {
            return;
        }

        // Optimistic check that we run the cleanup only on primary.
        // Note: It still can fail during the cleanup, but we will handle that error
        if (repl::ReplicationCoordinator::get(opCtx) == nullptr ||
            !repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary()) {
            return;
        }

        auto shardId = shardingState->shardId();

        // TODO(SERVER-126200): Remove reference to ChunklessPlaceholder.
        PersistentTaskStore<ChunkType> chunkStore{
            NamespaceString::kConfigShardCatalogChunksNamespace};
        try {
            chunkStore.remove(
                opCtx,
                BSON(ChunkType::shard()
                     << BSON("$nin" << BSON_ARRAY(
                                 shardId.toString()
                                 << shard_catalog_commit::kChunklessPlaceholderShardId.toString()))
                     << ChunkType::onCurrentShardSince() << BSON("$lt" << oldest)));
        } catch (const ExceptionFor<ErrorCodes::FailedToSatisfyReadPreference>&) {
            // Primary can be killed in the middle of the removal.
            return;
        } catch (const ExceptionFor<ErrorCodes::WriteConcernTimeout>&) {
            // Best-effort cleanup; retry on next pass.
            return;
        } catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
            // Ignore failAllRemoves failpoint
            if (ex.reason().find("failAllRemoves") != std::string::npos) {
                return;
            }
            // Otherwise, re-throw the internal error
            throw;
        } catch (const DBException& exception) {
            auto status = exception.toStatus();
            // Stepdown / primary change mid-removal; next oldest-timestamp pass will retry.
            if (ErrorCodes::isNotPrimaryError(status.code())) {
                return;
            }
            // Otherwise, re-throw the DBException
        }
    });
}  // namespace mongo::shard_catalog_helper
