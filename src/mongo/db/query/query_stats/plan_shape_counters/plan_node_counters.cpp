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

#include "mongo/db/query/query_stats/plan_shape_counters/plan_node_counters.h"

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

namespace mongo::plan_shape_counters {
namespace {

// ORs and SORT_MERGEs are counted separately depending on whether they have more children than
// this, since SBE is less performant for big plans.
constexpr size_t kManyChildrenThreshold = 100;

template <QsnNodeCounter small, QsnNodeCounter medium, QsnNodeCounter large>
QsnNodeCounter sizeBucket(long long n) {
    if (n <= 100) {
        return small;
    }
    if (n <= 10000) {
        return medium;
    }
    return large;
}
}  // namespace

void QsnNodeCountAnalyzer::preVisit(const QuerySolutionNode& node) {
    switch (node.getType()) {
        case STAGE_COLLSCAN:
            _counts.set(node.filter ? QsnNodeCounter::kCollscanWithFilter
                                    : QsnNodeCounter::kCollscanNoFilter);
            break;
        case STAGE_IXSCAN:
            _counts.set(node.filter ? QsnNodeCounter::kIxscanWithFilter
                                    : QsnNodeCounter::kIxscanNoFilter);
            break;
        case STAGE_FETCH:
            _counts.set(node.filter ? QsnNodeCounter::kFetchWithFilter
                                    : QsnNodeCounter::kFetchNoFilter);
            break;
        case STAGE_AND_HASH:
            _counts.set(node.filter ? QsnNodeCounter::kAndHashWithFilter
                                    : QsnNodeCounter::kAndHashNoFilter);
            break;
        case STAGE_AND_SORTED:
            _counts.set(QsnNodeCounter::kAndSorted);
            break;
        case STAGE_OR:
            if (node.children.size() <= kManyChildrenThreshold) {
                _counts.set(node.filter ? QsnNodeCounter::kOrWithFilterLte100Children
                                        : QsnNodeCounter::kOrNoFilterLte100Children);
            } else {
                _counts.set(node.filter ? QsnNodeCounter::kOrWithFilterGt100Children
                                        : QsnNodeCounter::kOrNoFilterGt100Children);
            }
            break;
        case STAGE_SORT_MERGE:
            if (node.children.size() <= kManyChildrenThreshold) {
                _counts.set(node.filter ? QsnNodeCounter::kSortMergeWithFilterLte100Children
                                        : QsnNodeCounter::kSortMergeNoFilterLte100Children);
            } else {
                _counts.set(node.filter ? QsnNodeCounter::kSortMergeWithFilterGt100Children
                                        : QsnNodeCounter::kSortMergeNoFilterGt100Children);
            }
            break;
        case STAGE_RETURN_KEY:
            _counts.set(QsnNodeCounter::kReturnKey);
            break;
        case STAGE_SHARDING_FILTER:
            _counts.set(QsnNodeCounter::kShardingFilter);
            break;
        case STAGE_PROJECTION_DEFAULT:
            _counts.set(QsnNodeCounter::kProjectionDefault);
            break;
        case STAGE_PROJECTION_COVERED:
            _counts.set(QsnNodeCounter::kProjectionCovered);
            break;
        case STAGE_PROJECTION_SIMPLE:
            _counts.set(QsnNodeCounter::kProjectionSimple);
            break;
        case STAGE_SORT_KEY_GENERATOR:
            _counts.set(QsnNodeCounter::kSortKeyGenerator);
            break;
        case STAGE_SORT_DEFAULT:
            _counts.set(static_cast<const SortNode&>(node).limit > 0
                            ? QsnNodeCounter::kSortDefaultWithLimit
                            : QsnNodeCounter::kSortDefaultNoLimit);
            break;
        case STAGE_SORT_SIMPLE:
            _counts.set(static_cast<const SortNode&>(node).limit > 0
                            ? QsnNodeCounter::kSortSimpleWithLimit
                            : QsnNodeCounter::kSortSimpleNoLimit);
            break;
        case STAGE_LIMIT:
            _counts.set(
                sizeBucket<QsnNodeCounter::kLimitSmall,
                           QsnNodeCounter::kLimitMedium,
                           QsnNodeCounter::kLimitLarge>(static_cast<const LimitNode&>(node).limit));
            break;
        case STAGE_SKIP:
            _counts.set(
                sizeBucket<QsnNodeCounter::kSkipSmall,
                           QsnNodeCounter::kSkipMedium,
                           QsnNodeCounter::kSkipLarge>(static_cast<const SkipNode&>(node).skip));
            break;
        case STAGE_TEXT_OR:
            _counts.set(QsnNodeCounter::kTextOr);
            break;
        case STAGE_MATCH:
            _counts.set(QsnNodeCounter::kMatch);
            break;
        case STAGE_REPLACE_ROOT:
            _counts.set(QsnNodeCounter::kReplaceRoot);
            break;
        case STAGE_GROUP:
            _counts.set(QsnNodeCounter::kGroup);
            break;
        case STAGE_EQ_LOOKUP:
            _counts.set(QsnNodeCounter::kEqLookupNoUnwind);
            break;
        case STAGE_EQ_LOOKUP_UNWIND:
            _counts.set(QsnNodeCounter::kEqLookupWithUnwind);
            break;
        case STAGE_UNPACK_TS_BUCKET:
            _counts.set(QsnNodeCounter::kUnpackTsBucket);
            break;
        case STAGE_HASH_JOIN_EMBEDDING_NODE:
            _counts.set(QsnNodeCounter::kHashJoin);
            break;
        case STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE:
            _counts.set(QsnNodeCounter::kNlj);
            break;
        case STAGE_INDEXED_NESTED_LOOP_JOIN_EMBEDDING_NODE:
            _counts.set(QsnNodeCounter::kInlj);
            break;
        case STAGE_INDEX_PROBE_NODE:
            _counts.set(QsnNodeCounter::kIndexProbe);
            break;
        // Untracked node types.
        // There is purposely no default case, so that adding a new stage type fails to compile
        // until it's given a counter or explicitly listed as uncounted.
        // If you are adding a new node type and would like it to be counted, add a new counter
        // to QsnNodeCounter. Otherwise, please add it to this list as an untracked type.
        case STAGE_BATCHED_DELETE:
        case STAGE_CACHED_PLAN:
        case STAGE_COLLSCAN_MULTI_RANGE:
        case STAGE_COUNT:
        case STAGE_COUNT_SCAN:
        case STAGE_DELETE:
        case STAGE_DISTINCT_SCAN:
        case STAGE_EOF:
        case STAGE_GEO_NEAR_2D:
        case STAGE_GEO_NEAR_2DSPHERE:
        case STAGE_IDHACK:
        case STAGE_MOCK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
        case STAGE_SEARCH:
        case STAGE_SENTINEL:
        case STAGE_SPOOL:
        case STAGE_SUBPLAN:
        case STAGE_TEXT_MATCH:
        case STAGE_TIMESERIES_MODIFY:
        case STAGE_TRIAL:
        case STAGE_UNKNOWN:
        case STAGE_UNPACK_SAMPLED_TS_BUCKET:
        case STAGE_UNWIND:
        case STAGE_UPDATE:
        case STAGE_VIRTUAL_SCAN:
        case STAGE_WINDOW:
            break;
    }
}

void QsnNodeCountAnalyzer::preVisit(query_solution_analyzer::RuleEngine&,
                                    const QuerySolutionNode& node,
                                    size_t) {
    preVisit(node);
}

}  // namespace mongo::plan_shape_counters
