// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_bitset_tree_converter.h"
#include "mongo/util/modules.h"

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
              /*maxNumPrimeImplicants*/ std::numeric_limits<size_t>::max(),
              /*maxSizeFactor*/ 1e6,
              /*doNotOpenContainedOrs*/ false,
              /*applyQuineMcCluskey*/ true) {}

    ExpressionSimplifierSettings(size_t maximumNumberOfUniquePredicates,
                                 size_t maximumNumberOfMinterms,
                                 size_t maxNumPrimeImplicants,
                                 double maxSizeFactor,
                                 bool doNotOpenContainedOrs,
                                 bool applyQuineMcCluskey)
        : maximumNumberOfUniquePredicates(maximumNumberOfUniquePredicates),
          maximumNumberOfMinterms(maximumNumberOfMinterms),
          maxNumPrimeImplicants(maxNumPrimeImplicants),
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
     * Petrick's method has exponential complexity. This causes us to avoid calling Petrick's method
     * when we have more than the specified upper limit of prime implicants.
     */
    size_t maxNumPrimeImplicants;

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
