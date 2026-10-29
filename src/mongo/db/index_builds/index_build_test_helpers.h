// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * Creates an index if it does not already exist.
 */
Status createIndex(OperationContext* opCtx,
                   std::string_view ns,
                   const BSONObj& keys,
                   bool unique = false);

/**
 * Creates an index from a BSON spec, if it does not already exist.
 */
Status createIndexFromSpec(OperationContext* opCtx, std::string_view ns, const BSONObj& spec);

/**
 * Creates an index from a BSON spec, if it does not already exist. If `clock` is non-null, writes
 * will be timestamped using the given clock. If it is null, they will be written with a fixed
 * timestamp.
 */
Status createIndexFromSpec(OperationContext* opCtx,
                           VectorClockMutable* clock,
                           std::string_view ns,
                           const BSONObj& spec);

Status initializeMultiIndexBlock(OperationContext* opCtx,
                                 CollectionWriter& collection,
                                 MultiIndexBlock& indexer,
                                 const BSONObj& spec,
                                 MultiIndexBlock::OnInitFn onInit = MultiIndexBlock::kNoopOnInitFn);
}  // namespace mongo
