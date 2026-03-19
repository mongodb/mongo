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

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"

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
 * Returns cumulative size and count deltas for each uuid across the inner operations of the
 * 'applyOpsEntry'.
 *
 * The OplogEntry provided must be of type 'repl::OplogEntry::CommandType::kApplyOps'; otherwise,
 * the method throws and terminates the current operation.
 */
stdx::unordered_map<UUID, CollectionSizeCount> extractSizeCountDeltasForApplyOps(
    const repl::OplogEntry& applyOpsEntry);

}  // namespace replicated_fast_count


}  // namespace mongo
