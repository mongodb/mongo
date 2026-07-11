// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo::oplog_truncation {

// Callback for truncating oplog entries within a marker range. Depending on the architecture,
// different truncation APIs should be used to ensure oplog truncation is properly replicated.
// Returns true if truncation succeeded and the marker should be popped, false otherwise.
using TruncateFn [[MONGO_MOD_PUBLIC]] =
    std::function<bool(OperationContext*, const CollectionTruncateMarkers::Marker&)>;

/**
 * Validates that oplog bounds are safe for truncation up to the given marker.
 * Checks that there exists a record after the marker but within the mayTruncateUpTo point.
 *
 * Returns true if validation passed and truncation can proceed, false otherwise.
 */
[[MONGO_MOD_PUBLIC]] bool checkOplogTruncationBounds(
    OperationContext* opCtx,
    RecordStore& oplog,
    const CollectionTruncateMarkers::Marker& marker,
    RecordId mayTruncateUpTo);

/**
 * Shared helper for iterating marker queue and validating cursor bounds before truncation.
 * Takes a caller-provided truncation function to handle implementation-specific truncation.
 *
 * Returns the highest RecordId that was truncated, or a null RecordId if nothing was truncated.
 */
[[MONGO_MOD_PUBLIC]] RecordId truncateByMarkerQueue(OperationContext* opCtx,
                                                    RecordStore& oplog,
                                                    RecordId mayTruncateUpTo,
                                                    TruncateFn truncateFn);

/**
 * Attempts to truncate oplog entries before the pinned oplog timestamp. Truncation will occur
 * if the oplog is at capacity and the maximum retention time has elapsed.
 *
 * `mayTruncateUpTo` is the highest allowable record that will be truncated, inclusive.
 *
 * Returns the highest RecordId that was truncated, or a null RecordId if nothing was truncated.
 */
RecordId reclaimOplog(OperationContext* opCtx, RecordStore& oplog, RecordId mayTruncateUpTo);

/**
 * Returns the upper bound, exclusive, of the timestamp that the oplog can be truncated to.
 *
 * If replicated fast count is enabled, this will be the minimum of the valid-as-of timestamp
 * persisted in the timestamp store and the storage engine's pinned oplog timestamp. Otherwise, this
 * will be the storage engine's pinned oplog timestamp.
 */
[[MONGO_MOD_PUBLIC]] Timestamp computeTruncationBound(OperationContext* opCtx);

}  // namespace mongo::oplog_truncation
