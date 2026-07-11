// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_utils.h"

namespace mongo::cost_based_ranker {

/**
 * Conditionally negate selectivity.
 */
template <bool negate>
constexpr SelectivityEstimate maybeNegate(const SelectivityEstimate s) {
    if constexpr (negate) {
        return s.negate();
    }
    return s;
}

/**
 * Computes conjunctive and disjunctive exponential backoff. We first take the extreme selectivities
 * (the smallest for conjunction, or the largest for disjunction). We then multiply them together
 * (inverting them for disjunction), and then for disjunction we invert the result, applying
 * increasing decay factor for each larger/smaller selectivity.
 */
template <bool isConjunction>
SelectivityEstimate expBackoffInternal(std::span<SelectivityEstimate> sels) {
    if (sels.size() == 1) {
        return sels[0];
    }

    const size_t actualMaxBackoffElements = std::min(sels.size(), kMaxBackoffElements);
    // Sort with an exact comparison: the estimates' approximate equivalence is non-transitive, so
    // it is not a strict weak ordering and would make std::partial_sort undefined behavior.
    std::partial_sort(sels.begin(),
                      sels.begin() + actualMaxBackoffElements,
                      sels.end(),
                      [](const SelectivityEstimate& a, const SelectivityEstimate& b) {
                          if constexpr (isConjunction) {
                              return exactLt(a, b);
                          } else {
                              return exactGt(a, b);
                          }
                      });

    SelectivityEstimate sel{SelectivityType{1.0}, EstimationSource::Code};
    double f = 1.0;
    for (size_t i = 0; i < actualMaxBackoffElements; i++, f /= 2.0) {
        // TODO: implement operator*=
        sel = sel * maybeNegate<!isConjunction>(sels[i]).pow(f);
    }
    return maybeNegate<!isConjunction>(sel);
}

SelectivityEstimate conjExponentialBackoff(std::span<SelectivityEstimate> conjSelectivities) {
    tassert(9582601,
            "The array of conjunction selectivities may not be empty.",
            !conjSelectivities.empty());
    return expBackoffInternal<true /*isConjunction*/>(conjSelectivities);
}

SelectivityEstimate disjExponentialBackoff(std::span<SelectivityEstimate> disjSelectivities) {
    tassert(9582602,
            "The array of disjunction selectivities may not be empty.",
            !disjSelectivities.empty());
    return expBackoffInternal<false /*isConjunction*/>(disjSelectivities);
}

void addFieldsToRelevantIndexOutput(const BSONObj& keyPattern, StringSet& relevantIndexOutput) {
    const auto& keyNames = keyPattern.getFieldNames<StringSet>();
    for (const auto& keyName : keyNames) {
        auto dotPos = keyName.find('.');
        relevantIndexOutput.insert(
            keyName.substr(0, dotPos != std::string::npos ? dotPos : keyName.size()));
    }
}

bool isNodeUnsupportedByCBR(StageType type) {
    // Once every node here is supported by CBR we should delete this function and any references to
    // it.
    switch (type) {
        case STAGE_COLLSCAN_MULTI_RANGE:  // TODO(130287): Add support for multi-range clustered
                                          // collection scans
        case STAGE_DISTINCT_SCAN:         // TODO SERVER-99075: Implement distinct scan
        case STAGE_TEXT_OR:
        case STAGE_TEXT_MATCH:
        case STAGE_GEO_NEAR_2D:
        case STAGE_GEO_NEAR_2DSPHERE:
        case STAGE_SORT_KEY_GENERATOR:
        case STAGE_RETURN_KEY: {
            return true;
        }
        case STAGE_BATCHED_DELETE:
        case STAGE_CACHED_PLAN:
        case STAGE_COUNT:
        case STAGE_COUNT_SCAN:
        case STAGE_DELETE:
        case STAGE_IDHACK:
        case STAGE_INDEXED_NESTED_LOOP_JOIN_EMBEDDING_NODE:
        case STAGE_INDEX_PROBE_NODE:
        case STAGE_HASH_JOIN_EMBEDDING_NODE:
        case STAGE_MATCH:
        case STAGE_MOCK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_REPLACE_ROOT:
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
        case STAGE_SPOOL:
        case STAGE_SUBPLAN:
        case STAGE_TIMESERIES_MODIFY:
        case STAGE_TRIAL:
        case STAGE_UNKNOWN:
        case STAGE_UNPACK_SAMPLED_TS_BUCKET:
        case STAGE_UNWIND:
        case STAGE_UPDATE:
        case STAGE_GROUP:
        case STAGE_EQ_LOOKUP:
        case STAGE_EQ_LOOKUP_UNWIND:
        case STAGE_SEARCH:
        case STAGE_WINDOW:
        case STAGE_SENTINEL:
        case STAGE_UNPACK_TS_BUCKET:
        case STAGE_COLLSCAN:
        case STAGE_VIRTUAL_SCAN:
        case STAGE_IXSCAN:
        case STAGE_FETCH:
        case STAGE_AND_HASH:
        case STAGE_AND_SORTED:
        case STAGE_OR:
        case STAGE_SORT_MERGE:
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE:
        case STAGE_SHARDING_FILTER:
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE:
        case STAGE_EOF:
        case STAGE_LIMIT:
        case STAGE_SKIP: {
            return false;
        }
    }
    MONGO_UNREACHABLE_TASSERT(12039703);
}

bool isNodeUnexpectedByCBR(StageType type) {
    switch (type) {
        case STAGE_BATCHED_DELETE:
        case STAGE_CACHED_PLAN:
        case STAGE_COLLSCAN_MULTI_RANGE:
        case STAGE_COUNT:
        case STAGE_COUNT_SCAN:
        case STAGE_DELETE:
        case STAGE_IDHACK:
        case STAGE_INDEXED_NESTED_LOOP_JOIN_EMBEDDING_NODE:
        case STAGE_INDEX_PROBE_NODE:
        case STAGE_HASH_JOIN_EMBEDDING_NODE:
        case STAGE_MATCH:
        case STAGE_MOCK:
        case STAGE_MULTI_ITERATOR:
        case STAGE_MULTI_PLAN:
        case STAGE_NESTED_LOOP_JOIN_EMBEDDING_NODE:
        case STAGE_QUEUED_DATA:
        case STAGE_RECORD_STORE_FAST_COUNT:
        case STAGE_REPLACE_ROOT:
        case STAGE_SAMPLE_FROM_TIMESERIES_BUCKET:
        case STAGE_SPOOL:
        case STAGE_SUBPLAN:
        case STAGE_TIMESERIES_MODIFY:
        case STAGE_TRIAL:
        case STAGE_UNKNOWN:
        case STAGE_UNPACK_SAMPLED_TS_BUCKET:
        case STAGE_UNWIND:
        case STAGE_UPDATE:
        case STAGE_GROUP:
        case STAGE_EQ_LOOKUP:
        case STAGE_EQ_LOOKUP_UNWIND:
        case STAGE_SEARCH:
        case STAGE_WINDOW:
        case STAGE_SENTINEL:
        case STAGE_UNPACK_TS_BUCKET: {
            return true;
        }
        case STAGE_SHARDING_FILTER:
        case STAGE_DISTINCT_SCAN:
        case STAGE_TEXT_OR:
        case STAGE_TEXT_MATCH:
        case STAGE_GEO_NEAR_2D:
        case STAGE_GEO_NEAR_2DSPHERE:
        case STAGE_SORT_KEY_GENERATOR:
        case STAGE_RETURN_KEY:
        case STAGE_COLLSCAN:
        case STAGE_VIRTUAL_SCAN:
        case STAGE_IXSCAN:
        case STAGE_FETCH:
        case STAGE_AND_HASH:
        case STAGE_AND_SORTED:
        case STAGE_OR:
        case STAGE_SORT_MERGE:
        case STAGE_SORT_DEFAULT:
        case STAGE_SORT_SIMPLE:
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE:
        case STAGE_EOF:
        case STAGE_LIMIT:
        case STAGE_SKIP: {
            return false;
        }
    }
    MONGO_UNREACHABLE_TASSERT(12039704);
}
}  // namespace mongo::cost_based_ranker
