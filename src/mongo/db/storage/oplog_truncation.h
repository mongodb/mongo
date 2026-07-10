/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
