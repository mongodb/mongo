// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/plan_shape_counters/plan_access_path_counters.h"

#include "mongo/db/index_names.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

#include <algorithm>
#include <vector>

namespace mongo::plan_shape_counters {
namespace {

// Threshold for how many bounds are needed to be marked as `kBoundsUnionedLarge` instead of
// `kBoundsUnionedSmall`. This value is included in the large category.
constexpr size_t kUnionedBoundsLargeThreshold = 51;

void classifyIndexType(AccessPathCounts& counts, const IndexEntry& index) {
    if (index.type == INDEX_BTREE) {
        counts.set(AccessPathCounter::kBtreeIxscan);
    }
    if (index.type == INDEX_WILDCARD) {
        counts.set(AccessPathCounter::kWildcardIxscan);
    }
    if (index.type == INDEX_HASHED) {
        counts.set(AccessPathCounter::kHashedIxscan);
    }
    if (index.sparse) {
        counts.set(AccessPathCounter::kSparseIxscan);
    }
    if (index.unique) {
        counts.set(AccessPathCounter::kUniqueIxscan);
    }
    if (index.multikey) {
        counts.set(AccessPathCounter::kMultikeyIxscan);
    }
}

AccessPathCounter classifyBoundsInterval(bool unboundedBelow, bool unboundedAbove, bool isPoint) {
    if (isPoint) {
        return AccessPathCounter::kBoundsPoint;
    } else if (unboundedBelow && unboundedAbove) {
        return AccessPathCounter::kBoundsFullScan;
    } else if (unboundedBelow) {
        return AccessPathCounter::kBoundsMinKeyToValue;
    } else if (unboundedAbove) {
        return AccessPathCounter::kBoundsValueToMaxKey;
    } else {
        return AccessPathCounter::kBoundsBoundedRange;
    }
}

// Classifies the single interval [start, end] bounding one field of an index scan.
AccessPathCounter classifyBoundsInterval(const BSONElement& start, const BSONElement& end) {
    bool hasMinKey = start.type() == BSONType::minKey || end.type() == BSONType::minKey;
    bool hasMaxKey = start.type() == BSONType::maxKey || end.type() == BSONType::maxKey;
    bool isPoint = start.woCompare(end, false) == 0;
    return classifyBoundsInterval(hasMinKey, hasMaxKey, isPoint);
}

// Classify the bounds of a clustered collscan.
void classifyCollscanBounds(AccessPathCounts& counts,
                            const boost::optional<RecordIdBound>& minRecord,
                            const boost::optional<RecordIdBound>& maxRecord) {
    bool isPoint = minRecord && maxRecord && *minRecord == *maxRecord;
    counts.set(classifyBoundsInterval(!minRecord, !maxRecord, isPoint));
}

// Classify the bounds of an ixscan.
void classifyIndexBounds(AccessPathCounts& counts, const IndexBounds& bounds) {
    std::vector<AccessPathCounter> boundTypes;
    if (bounds.isSimpleRange) {
        // Simple range scans are encoded as startKey/endKey BSON rather than with an OIL.
        BSONObjIterator startIt(bounds.startKey);
        BSONObjIterator endIt(bounds.endKey);
        while (startIt.more() && endIt.more()) {
            boundTypes.push_back(classifyBoundsInterval(startIt.next(), endIt.next()));
        }
    } else {
        // Iterate through each field in the index scan, and then through every
        // interval that field can take on.
        for (auto&& oil : bounds.fields) {
            // Mark counters related to number of unioned index bounds
            if (oil.intervals.size() >= kUnionedBoundsLargeThreshold) {
                counts.set(AccessPathCounter::kBoundsUnionedLarge);
            } else if (oil.intervals.size() >= 2) {
                counts.set(AccessPathCounter::kBoundsUnionedSmall);
            }

            for (auto&& interval : oil.intervals) {
                boundTypes.push_back(classifyBoundsInterval(interval.start, interval.end));
            }
        }
    }

    // If no bounds were found, there are no counters to set.
    if (boundTypes.empty()) {
        return;
    }

    // If all bounds were the same type, we increment that type. Otherwise, increment
    // `kBoundsMixture`.
    bool sameTypesOfBounds =
        std::all_of(boundTypes.begin(), boundTypes.end(), [&](AccessPathCounter type) {
            return type == boundTypes.front();
        });
    if (sameTypesOfBounds) {
        counts.set(boundTypes.front());
    } else {
        counts.set(AccessPathCounter::kBoundsMixture);
    }
}
}  // namespace

void AccessPathAnalyzer::preVisit(query_solution_analyzer::RuleEngine&,
                                  const QuerySolutionNode& node,
                                  size_t) {
    switch (node.getType()) {
        case STAGE_COLLSCAN: {
            const auto& csn = static_cast<const CollectionScanNode&>(node);
            bool isClustered =
                csn.doClusteredCollectionScanClassic() || csn.doClusteredCollectionScanSbe();
            _counts.set(isClustered ? AccessPathCounter::kClusteredCollscan
                                    : AccessPathCounter::kCollscan);
            if (isClustered) {
                classifyCollscanBounds(_counts, csn.minRecord, csn.maxRecord);
            }
            break;
        }
        case STAGE_DISTINCT_SCAN: {
            const auto& distinctNode = static_cast<const DistinctNode&>(node);
            // A distinct scan can fetch by itself or under a FETCH stage.
            const bool fetched = isNodeFetched() || distinctNode.isFetching;
            _counts.set(fetched ? AccessPathCounter::kDistinctScanFetch
                                : AccessPathCounter::kDistinctScan);
            break;
        }
        case STAGE_IXSCAN: {
            const auto& isn = static_cast<const IndexScanNode&>(node);
            _counts.set(isNodeFetched() ? AccessPathCounter::kIxscanFetch
                                        : AccessPathCounter::kCoveredIxscan);
            classifyIndexType(_counts, isn.index);
            classifyIndexBounds(_counts, isn.bounds);
            break;
        }
        case STAGE_COUNT_SCAN:
            _counts.set(AccessPathCounter::kCountScan);
            break;
        case STAGE_FETCH:
            ++_fetchDepth;
            break;
        case STAGE_GEO_NEAR_2D:
            _counts.set(AccessPathCounter::kGeoNear2d);
            break;
        case STAGE_GEO_NEAR_2DSPHERE:
            _counts.set(AccessPathCounter::kGeoNear2dSphere);
            break;
        case STAGE_TEXT_MATCH:
            _counts.set(AccessPathCounter::kTextMatch);
            break;
        default:
            break;
    }
}

void AccessPathAnalyzer::postVisit(query_solution_analyzer::RuleEngine&,
                                   const QuerySolutionNode& node) {
    if (node.getType() == STAGE_FETCH) {
        --_fetchDepth;
    }
}

void AccessPathAnalyzer::finish(query_solution_analyzer::RuleEngine& engine) {
    static const auto accessPathCounters = {AccessPathCounter::kCollscan,
                                            AccessPathCounter::kClusteredCollscan,
                                            AccessPathCounter::kCoveredIxscan,
                                            AccessPathCounter::kCountScan,
                                            AccessPathCounter::kDistinctScan,
                                            AccessPathCounter::kIxscanFetch,
                                            AccessPathCounter::kDistinctScanFetch,
                                            AccessPathCounter::kGeoNear2d,
                                            AccessPathCounter::kGeoNear2dSphere};
    // If no access path was found, mark `kOtherAccessPath`.
    if (std::ranges::none_of(accessPathCounters,
                             [&](AccessPathCounter c) { return _counts.test(c); })) {
        _counts.set(AccessPathCounter::kOtherAccessPath);
    }
}
}  // namespace mongo::plan_shape_counters
