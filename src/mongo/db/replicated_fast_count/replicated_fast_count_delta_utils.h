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
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace replicated_fast_count {

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
 * Aggregates per-collection size and count deltas across a list of operations. Returns one
 * `MultiOpSizeMetadata` entry per collection UUID touched. Operations without size metadata
 * (`m` field) are skipped.
 */
MONGO_MOD_PUBLIC std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadata(
    const std::vector<repl::ReplOperation>& ops);

MONGO_MOD_PUBLIC std::vector<MultiOpSizeMetadata> aggregateMultiOpSizeMetadata(
    const std::vector<repl::OplogEntry>& ops);

/**
 * Accumulates cumulative size and count deltas for each uuid across the inner operations of the
 * 'applyOpsEntry' into 'sizeCountDeltasOut'. If 'uuidFilter' is provided, only entries for that
 * UUID are collected. Returns the number of size/count entries processed.
 *
 * The OplogEntry provided must be of type 'repl::OplogEntry::CommandType::kApplyOps'; otherwise,
 * the method throws and terminates the current operation.
 */
int extractSizeCountDeltasForApplyOps(const repl::OplogEntry& applyOpsEntry,
                                      const boost::optional<UUID>& uuidFilter,
                                      SizeCountDeltas& sizeCountDeltasOut);

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
    SizeCountDeltas deltas;
    boost::optional<Timestamp> lastTimestamp;
};

/**
 * Given a cursor to the oplog, scans the oplog starting after "seekAfterTS" (exclusive bound) and
 * aggregates the size count deltas across UUIDs. Only accumulates size count information for
 * "uuidFilter" when provided. Pass 'isCheckpoint=true' only on the checkpoint scan path to
 * increment checkpoint scan counters; leave false (the default) on read paths.
 *
 * If a non-none 'oplogUuid' is passed in, tracks updates to the oplog size and count.
 */
OplogScanResult aggregateSizeCountDeltasInOplog(
    SeekableRecordCursor& oplogCursor,
    const Timestamp& seekAfterTS,
    const boost::optional<UUID>& uuidFilter = boost::none,
    bool isCheckpoint = false,
    const boost::optional<UUID>& oplogUuid = boost::none);

}  // namespace replicated_fast_count


}  // namespace mongo
