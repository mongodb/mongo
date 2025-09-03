/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"

namespace mongo::ce {

enum class NoProjection {};

using TopLevelFieldsProjection = StringSet;

/**
 * std::variant type used to specify whether we should project fields when generating the sample.
 */
using ProjectionParams = std::variant<NoProjection, TopLevelFieldsProjection>;

using CardinalityEstimate = mongo::cost_based_ranker::CardinalityEstimate;

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
};

}  // namespace mongo::ce
