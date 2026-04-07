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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/uuid.h"

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace replicated_fast_count {

inline constexpr StringData kMetadataKey = "meta"_sd;
inline constexpr StringData kSizeKey = "sz"_sd;
inline constexpr StringData kCountKey = "ct"_sd;
inline constexpr StringData kValidAsOfKey = "valid-as-of"_sd;

/**
 * Returns the size and count delta extracted from the oplog entry's size metadata ('m' field), if
 * present.
 *
 * This function expects to be called on an oplog entry for a single operation. For 'applyOps'
 * entries, the top-level entry cannot have an 'm' (size metadata) field; however, the inner
 * operations within the 'applyOps' array can and should be parsed separately.
 */
boost::optional<CollectionSizeCount> extractSizeCountDeltaForOp(const repl::OplogEntry& oplogEntry);

/**
 * Accumulates cumulative size and count deltas for each uuid across the inner operations of the
 * 'applyOpsEntry' into 'sizeCountDeltasOut'. If 'uuidFilter' is provided, only entries for that
 * UUID are collected.
 *
 * The OplogEntry provided must be of type 'repl::OplogEntry::CommandType::kApplyOps'; otherwise,
 * the method throws and terminates the current operation.
 */
void extractSizeCountDeltasForApplyOps(
    const repl::OplogEntry& applyOpsEntry,
    const boost::optional<UUID>& uuidFilter,
    absl::flat_hash_map<UUID, CollectionSizeCount>& sizeCountDeltasOut);

/**
 * The result of scanning the oplog for size and count deltas.
 *
 * `deltas` contains an entry for each `uuid` which has replicated size count information within the
 * scanned oplog range. May include entries where size count deltas sum to 0.
 *
 * `lastTimestamp` is the timestamp of the final oplog entry visited during the scan that is NOT
 * from an internal fast count store collection, or boost::none if no such entries were scanned
 * (i.e. the seek landed past the end of the oplog).
 */
struct OplogScanResult {
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    boost::optional<Timestamp> lastTimestamp;
};

/**
 * Given a cursor to the oplog, scans the oplog starting after "seekAfterTS" (exclusive bound) and
 * aggregates the size count deltas across UUIDs. Only accumulates size count information for
 * "uuidFilter" when provided.
 */
OplogScanResult aggregateSizeCountDeltasInOplog(
    SeekableRecordCursor& oplogCursor,
    const Timestamp& seekAfterTS,
    const boost::optional<UUID>& uuidFilter = boost::none);

/**
 * Acquires the replicated fast count collection for read access.
 * Returns boost::none if the collection does not exist.
 */
boost::optional<CollectionOrViewAcquisition> acquireFastCountCollectionForRead(
    OperationContext* opCtx);

/**
 * Acquire the fastcount collection that underpins this class with write intent.
 * Returns boost::none if it doesn't exist.
 */
boost::optional<CollectionOrViewAcquisition> acquireFastCountCollectionForWrite(
    OperationContext* opCtx);

/**
 * For each entry in 'deltas', looks up the persisted size and count for that UUID in the
 * on-disk fast count collection and adds the persisted values to the entry's size and count
 * in place. If a UUID has no on-disk entry, its delta is left unchanged.
 */
void readAndIncrementSizeCounts(OperationContext* opCtx,
                                absl::flat_hash_map<UUID, CollectionSizeCount>& deltas);
}  // namespace replicated_fast_count


}  // namespace mongo
