// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/util/uuid.h"

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

/**
 * Type alias for the map of collection UUID to optional metadata. `CollectionReplicatedMetadata` is
 * optional because when a checkpoint contains a collection delete entry, there is no replicated
 * metadata to check.
 */
using CheckpointContents = absl::flat_hash_map<UUID, boost::optional<CollectionReplicatedMetadata>>;

enum class ReportingStrategy { kFassert, kLogError };

/**
 * Validates the set equality of the `standby` and `primary` replicated metadata checkpoints.
 *
 * If `reportingStrategy` is `Fassert`, the function terminates the process immediately on the first
 * inequality detected. If `reportingStrategy` is `LogError`, the function logs an error for each
 * inequality and continues.
 */
void validateCheckpoint(const CheckpointContents& standby,
                        const CheckpointContents& primary,
                        ReportingStrategy reportingStrategy);

/**
 * Returns true if the provided oplog entry is a replicated metadata checkpoint, and false
 * otherwise.
 */
bool isCheckpointApplyOpsEntry(const repl::OplogEntry& entry);

/**
 * Parses a replicated metadata checkpoint entry and returns the per-collection replicated metadata
 * it contains.
 */
// TODO(SERVER-136501): Handle chained applyOps entries.
CheckpointContents parseCheckpoint(const repl::OplogEntry& entry);

}  // namespace mongo::replicated_fast_count
