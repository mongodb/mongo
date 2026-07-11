// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/write_ops/query_stats_metrics_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
/**
 * Returns an OpMsgRequest for the supplied db and cmd. If useDocSequence is true, it will move the
 * following fields from the body to a document sequence:
 *      "documents", "updates", "deletes", "GARBAGE"
 *
 * This is intended to be used like this:
 *
 * const auto cmdObj = BSON( ... );
 * for (auto docSeq : {false, true}) {
 *     auto req = parse(toOpMsg("test", cmdObj, docSeq));
 *     ASSERT(...);
 * }
 */
OpMsgRequest toOpMsg(std::string_view db, const BSONObj& cmd, bool useDocSequence);

/**
 * Helper to create a QueryStatsMetrics object with all required fields populated.
 */
write_ops::QueryStatsMetrics makeQueryStatsMetrics(int originalOpIndex,
                                                   long long keysExamined,
                                                   long long docsExamined,
                                                   long long nMatched,
                                                   long long nInserted = 0);

}  // namespace mongo
