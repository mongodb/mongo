/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_bitset_tree_converter.h"

namespace mongo {
/**
 * Class containing metrics about simplification of boolean expressions.
 */
struct ExpressionSimplifierMetrics {
    /**
     * Amount of times the simplifier did not run due to a trivial filter
     */
    Counter64& trivialCount = *MetricBuilder<Counter64>{"query.expressionSimplifier.trivial"};
    /**
     * Amount of times the simplifier aborted because the resulting expression became too large
     */
    Counter64& abortedTooLargeCount =
        *MetricBuilder<Counter64>{"query.expressionSimplifier.abortedTooLarge"};
    /**
     * Amount of times the simplifier completed without simplifying the expression
     */
    Counter64& notSimplifiedCount =
        *MetricBuilder<Counter64>{"query.expressionSimplifier.notSimplified"};
    /**
     * Amount of times the simplifier completed and simplified the expression
     */
    Counter64& simplifiedCount = *MetricBuilder<Counter64>{"query.expressionSimplifier.simplified"};
};
extern ExpressionSimplifierMetrics expressionSimplifierMetrics;

/**
 * Settings to control simplification of boolean expressions.
 */
struct ExpressionSimplifierSettings {
    /**
     * Default constructor with minimum restrictions on boolean expression simplification. Useful
     * for tests and benchmarks.
     */
    ExpressionSimplifierSettings()
        : ExpressionSimplifierSettings(
              /*maximumNumberOfUniquePredicates*/ std::numeric_limits<size_t>::max(),
              /*maximumNumberOfMinterms*/ std::numeric_limits<size_t>::max(),
              /*maxSizeFactor*/ 1e6,
              /*doNotOpenContainedOrs*/ false,
              /*applyQuineMcCluskey*/ true) {}

    ExpressionSimplifierSettings(size_t maximumNumberOfUniquePredicates,
                                 size_t maximumNumberOfMinterms,
                                 double maxSizeFactor,
                                 bool doNotOpenContainedOrs,
                                 bool applyQuineMcCluskey)
        : maximumNumberOfUniquePredicates(maximumNumberOfUniquePredicates),
          maximumNumberOfMinterms(maximumNumberOfMinterms),
          maxSizeFactor(maxSizeFactor),
          doNotOpenContainedOrs(doNotOpenContainedOrs),
          applyQuineMcCluskey(applyQuineMcCluskey) {}

    /**
     * If the number of unique predicates in an expression is larger than
     * 'maximumNumberOfUniquePredicates' the expression is considered too big to be simplified.
     */
    size_t maximumNumberOfUniquePredicates;

    /**
     * Maximum number of minterms allowed during boolean transformations.
     */
    size_t maximumNumberOfMinterms;

    /**
     * If the simplified expression is larger than the original expression's size times
     * `maxSizeFactor`, the simplified one will be rejected.
     */
    double maxSizeFactor;

    /**
     * If the original expression contains AND operator it is still simplified but the common
     * predicate of the simplified conjunctive terms are taken out of brackets.
     */
    bool doNotOpenContainedOrs;

    /**
     * If the parameter is false we only convert the input expression into DNF form without applying
     * the Quine–McCluskey algorithm.
     */
    bool applyQuineMcCluskey;
};

/**
 * Returns simplified MatchExpression if it is possible under conditions specified in the second
 * parameter 'settings'.
 */
boost::optional<std::unique_ptr<MatchExpression>> simplifyMatchExpression(
    const MatchExpression* root, const ExpressionSimplifierSettings& settings);
}  // namespace mongo
