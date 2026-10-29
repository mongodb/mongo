// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo::change_streams {
/**
 * Issues the required cursor open/close requests on the change stream reader context based on the
 * difference between 'newActiveShards' and the set of cursors currently tracked by the reader
 * context.
 */
void updateActiveShardCursors(Timestamp atClusterTime,
                              const std::vector<ShardId>& newActiveShards,
                              ChangeStreamReaderContext& readerCtx);

/**
 * Validates that the status of the historical placement result is 'OK', and tasserts otherwise.
 */
void assertHistoricalPlacementStatusOK(const HistoricalPlacement& placement);

/**
 * Validates that the 'openCursorAt' and 'nextPlacementChangedAt' fields of the historical
 * placement result are not set, and tasserts otherwise.
 */
void assertHistoricalPlacementHasNoSegment(const HistoricalPlacement& placement);

/**
 * Returns a pointer to the data to shards allocation query service from the context, asserting
 * if one is not set.
 */
DataToShardsAllocationQueryService* getDataToShardsAllocationQueryService(OperationContext* opCtx);

}  // namespace mongo::change_streams
