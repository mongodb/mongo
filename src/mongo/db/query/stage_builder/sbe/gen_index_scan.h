/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/storage/key_string/key_string.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {

class PlanStageReqs;
class PlanStageSlots;

constexpr StringData kIdIndexName = IndexConstants::kIdIndexName;

/**
 * A list of low and high key values representing ranges over a particular index.
 */
using IndexIntervals =
    std::vector<std::pair<std::unique_ptr<key_string::Value>, std::unique_ptr<key_string::Value>>>;

/**
 * This method returns a pair containing: (1) an SBE plan stage tree implementing an index scan;
 * and (2) a PlanStageSlots object containing a kRecordId slot, possibly some other kMeta slots,
 * and slots produced by the index scan that were required by 'reqs'.
 */
std::pair<SbStage, PlanStageSlots> generateIndexScan(StageBuilderState& state,
                                                     const CollectionPtr& collection,
                                                     const IndexScanNode* ixn,
                                                     const PlanStageReqs& reqs);

/**
 * Constructs the most simple version of an index scan from the single interval index bounds.
 * 'isPointInterval' indicates if the single interval is a point interval.
 *
 * In case when the 'lowKey'/'lowKeySlot' and 'highKey' are not specified, slots will be registered
 * for them in the runtime environment and their slot ids returned as a pair in the third element of
 * the tuple.
 *
 * If 'indexKeySlot' is provided, than the corresponding slot will be filled out with each KeyString
 * in the index.
 */
std::tuple<SbStage, PlanStageSlots, boost::optional<std::pair<SbSlot, SbSlot>>>
generateSingleIntervalIndexScanAndSlots(StageBuilderState& state,
                                        const CollectionPtr& collection,
                                        const std::string& indexName,
                                        const BSONObj& keyPattern,
                                        bool forward,
                                        std::unique_ptr<key_string::Value> lowKey,
                                        std::unique_ptr<key_string::Value> highKey,
                                        const PlanStageReqs& reqs,
                                        PlanNodeId planNodeId,
                                        bool isPointInterval);

std::pair<SbStage, PlanStageSlots> generateSingleIntervalIndexScan(StageBuilderState& state,
                                                                   const CollectionPtr& collection,
                                                                   const std::string& indexName,
                                                                   const BSONObj& keyPattern,
                                                                   bool forward,
                                                                   SbExpr lowKeyExpr,
                                                                   SbExpr highKeyExpr,
                                                                   const PlanStageReqs& reqs,
                                                                   PlanNodeId planNodeId);

/**
 * Constructs low/high key values from the given index 'bounds' if they can be represented either as
 * a single interval between the low and high keys, or multiple single intervals. If index bounds
 * for some interval cannot be expressed as valid low/high keys, then an empty vector is returned.
 */
IndexIntervals makeIntervalsFromIndexBounds(const IndexBounds& bounds,
                                            bool forward,
                                            key_string::Version version,
                                            Ordering ordering);

/**
 * Construct an array containing objects with the low and high keys for each interval.
 *
 * E.g., [ {l: KS(...), h: KS(...)},
 *         {l: KS(...), h: KS(...)}, ... ]
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> packIndexIntervalsInSbeArray(
    IndexIntervals intervals);

/**
 * Constructs a generic multi-interval index scan. Depending on the intervals will either execute
 * the optimized or the generic index scan subplan.
 *
 * This method returns a pair containing: (1) an SBE plan stage tree implementing a generic multi-
 * interval index scan; and (2) a PlanStageSlots object containing a kRecordId slot, possibly some
 * other kMeta slots, and slots produced by the index scan that were required by 'reqs'.
 */
std::pair<SbStage, PlanStageSlots> generateIndexScanWithDynamicBounds(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const PlanStageReqs& reqs);

/**
 * Checks if 'iets' resolves to a single point interval. It must be a sequence of '$eq' or constant
 * point intervals.
 */
bool ietsArePointInterval(const std::vector<interval_evaluation_tree::IET>& iets);

}  // namespace mongo::stage_builder
