// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/util/modules.h"

namespace mongo::ce {

enum class NoProjection {};

using TopLevelFieldsProjection = StringSet;

/**
 * std::variant type used to specify whether we should project fields when generating the sample.
 */
using ProjectionParams = std::variant<NoProjection, TopLevelFieldsProjection>;

using CardinalityEstimate = mongo::cost_based_ranker::CardinalityEstimate;
using SamplingMetadata = mongo::cost_based_ranker::SamplingMetadata;

/**
 * One persisted NDV statistic (analyze mode "ndv") that served an estimate: the (sorted) field
 * paths it describes and the statistics document's creation time. Deliberately not part of
 * SamplingMetadata: NDV statistics are field statistics, unrelated to the sample, even though
 * the sampling estimator serves both.
 */
struct PersistedNDVEntry {
    std::vector<std::string> sortedFieldPaths;
    // When 'analyze' built this statistic; one namespace may serve several NDV statistics, each
    // analyzed at its own time.
    Date_t createdAt;
    // The NDV of each persisted sketch variant, in the persisted order (see NdvStats in
    // field_stats.idl). Lets the reader cache a loaded statistic compactly, without the
    // 16KB-per-sketch registers.
    std::vector<long long> ndvPerSketch;
};

class SamplingEstimator {
public:
    virtual ~SamplingEstimator() {}

    /**
     * Estimates the Cardinality of a filter/MatchExpression by running the given ME against the
     * sample.
     */
    virtual CardinalityEstimate estimateCardinality(const MatchExpression* expr) const = 0;

    /**
     * Batch Estimates the Cardinality of a vector of filter/MatchExpression by running the given
     * MEs against the sample.
     */
    virtual std::vector<CardinalityEstimate> estimateCardinality(
        const std::vector<const MatchExpression*>& expr) const = 0;

    /**
     * Estimates the number of keys scanned for the given IndexBounds.
     */
    virtual CardinalityEstimate estimateKeysScanned(const IndexBounds& bounds) const = 0;

    /**
     * Batch estimates the number of keys scanned for the given vector of IndexBounds.
     */
    virtual std::vector<CardinalityEstimate> estimateKeysScanned(
        const std::vector<const IndexBounds*>& bounds) const = 0;

    /**
     * Estimates the number of RIDs matched the given IndexBounds and an optional MatchExpression.
     * 'expr' can be nullptr to indicate only estimate the index bounds. When 'expr' is provided
     * the 'expr' will be evaluated against the documents that fall into the 'bounds'.
     */
    virtual CardinalityEstimate estimateRIDs(const IndexBounds& bounds,
                                             const MatchExpression* expr) const = 0;

    /**
     * Batch estimates the number of RIDs matched the given IndexBounds. 'expressions' can be
     * nullptr to indicate only estimate the index bounds. 'bounds' and 'expressions' come in pairs
     * so they should have the same size.
     */
    virtual std::vector<CardinalityEstimate> estimateRIDs(
        const std::vector<const IndexBounds*>& bounds,
        const std::vector<const MatchExpression*>& expressions) const = 0;

    virtual void generateSample(ce::ProjectionParams projectionParams) = 0;

    /**
     * Estimates the number of distinct values of tuples of the given field names in the collection.
     * Does not support estimating NDV over array-valued fields.
     * 'fields' specifies which fields should follow strict, $expr-style equality (null !=
     * missing) vs. regular equality semantics (null == missing).
     *
     * Note: when the estimate is served from persisted NDV statistics (no bounds, at most
     * kNdvMaxFields fields), the semantics select the persisted folding variant. A single-field
     * statistic carries only the strict ($expr) sketch and serves both semantics, off by at most
     * one; a composite statistic carries one variant per folded field, and requests folding
     * several fields fall back to the sample.
     */
    virtual CardinalityEstimate estimateNDV(
        const std::vector<FieldPathAndEqSemantics>& fields,
        boost::optional<std::span<const OrderedIntervalList>> bounds) const = 0;

    CardinalityEstimate estimateNDV(const std::vector<FieldPathAndEqSemantics>& fields) const {
        return estimateNDV(fields, boost::none);
    }

    /**
     * Estimates the number of distinct values of tuples of the given field names
     * a multikey index over the provided fields would contain, from the values
     * present in the sample.
     *
     * The sample documents are drawn from the collection; this is not exactly equivalent
     * to sampling from a multikey index, as each index key is not equally weighted or
     * drawn independently of "sibling" values in the same document.
     */
    virtual CardinalityEstimate estimateNDVMultiKey(
        const std::vector<FieldPathAndEqSemantics>& fields,
        boost::optional<std::span<const OrderedIntervalList>> bounds) const = 0;

    CardinalityEstimate estimateNDVMultiKey(
        const std::vector<FieldPathAndEqSemantics>& fields) const {
        return estimateNDVMultiKey(fields, boost::none);
    }

    virtual CardinalityEstimate getCollCard() const = 0;

    virtual size_t getSampleSize() const = 0;

    /**
     * Returns metadata about the sample used for cardinality estimation.
     */
    virtual SamplingMetadata getSamplingMetadata() const = 0;

    /**
     * Returns one entry per persisted NDV statistic (analyze mode "ndv") that served an
     * estimate, sorted by field paths. Kept apart from getSamplingMetadata(): these are field
     * statistics, not derived from the sample, even though this estimator serves both.
     */
    virtual std::vector<PersistedNDVEntry> getPersistedNDVMetadata() const = 0;

    /**
     * Returns the number of distinct persisted NDV statistics (analyze mode "ndv") this estimator
     * has served so far. Repeated NDV requests for the same field path are memoized and counted
     * once. Cheaper than getPersistedNDVMetadata() when only the count is needed.
     */
    virtual size_t getNumPersistedNDVStatsUsed() const {
        return 0;
    }
};

}  // namespace mongo::ce
