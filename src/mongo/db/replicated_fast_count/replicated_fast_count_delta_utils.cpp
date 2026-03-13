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

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

bool isSupportedOpType(repl::OpTypeEnum opType) {
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
        case repl::OpTypeEnum::kUpdate:
        case repl::OpTypeEnum::kDelete:
            return true;
        default:
            return false;
    }
}

//  Given the 'opType' of an oplog entry which replicates 'sz' (size delta) information, compute
//  the 'count' delta.
int32_t computeCountDeltaForOp(repl::OpTypeEnum opType) {
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
            return 1;
        case repl::OpTypeEnum::kUpdate:
            return 0;
        case repl::OpTypeEnum::kDelete:
            return -1;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

namespace replicated_fast_count {
boost::optional<CollectionSizeCount> extractSizeCountDeltaForOp(
    const repl::OplogEntry& oplogEntry) {
    const auto& sizeMd = oplogEntry.getSizeMetadata();
    if (!sizeMd) {
        return boost::none;
    }

    // The 'm' field in the oplog entry contains the replicated size delta.  The collection count
    // delta must be inferred from the operation type. Throw if the operation type is not supported
    // for size/count tracking.
    const auto& opType = oplogEntry.getOpType();
    massert(12115900,
            str::stream() << "Unexpected input: Operation type '" << idl::serialize(opType)
                          << "' incompatible with top level 'm' field: "
                          << redact(oplogEntry.toBSONForLogging()),
            isSupportedOpType(opType));

    massert(12115901,
            str::stream() << "Unexpected input: Namespace '"
                          << oplogEntry.getNss().toStringForErrorMsg()
                          << "' is incompatible with top level 'm' field: "
                          << redact(oplogEntry.toBSONForLogging()),
            isReplicatedFastCountEligible(oplogEntry.getNss()));
    const int32_t sizeDelta = sizeMd->getSz();
    const int32_t countDelta = computeCountDeltaForOp(opType);
    return CollectionSizeCount{.size = sizeDelta, .count = countDelta};
}
}  // namespace replicated_fast_count


}  // namespace mongo
