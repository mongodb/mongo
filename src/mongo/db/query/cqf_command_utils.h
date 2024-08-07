/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <iostream>
#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

/**
 * Eligibility is a measure of increasingly smaller subsets of queries. We generally assume the
 * query is in the smallest subset and evaluate a set of predicates which may prove query is not a
 * member of the currently assumed subset causing the eligibility to be reduced to a larger set. The
 * eligibility of a query is the smallest set which the query is a member of.
 *
 * The largest set, the set of all queries (aka Ineligible), includes not only those queries which
 * are supported by Bonsai in some capacity, but those which are not supported by Bonsai at all. The
 * next smaller set contains queries which are experimentally supported in Bonsai. They will not be
 * executed by Bonsai in a release configuration but are eligible in test configurations.
 * The smallest subset (aka FullyEligible) is the set of queries that execute correctly and
 * efficiently.
 *
 * This structure is a mutable object representing an eligibility level. It provides a fluent style
 * API which facilitates combining multiple constraints to compute the eligibility, eg.
 *
 * eligibility.setIneligibleIf(condition1)
 *            .minOf([&]() { return expensiveCheck(); });
 */
struct BonsaiEligibility {
    enum Eligibility {
        // Ordered as increasingly smaller sets to facilitate comparisons.
        /**
         * "Ineligible": The complete set of all possible queries.
         */
        Ineligible = 0,
        /**
         * "Experimentally eligible": A level more constrained than "Ineligible" is the set of
         * queries experimentally supported.
         */
        Experimental,
        /**
         * "Fully eligible": This is the smallest subset of queries including those which are
         * eligible under all current constraints.
         */
        FullyEligible,
    };

    BonsaiEligibility(Eligibility e, Eligibility min = Ineligible)
        : _eligibility(e), _minEligibility(min) {}

    /**
     * The max eligibility. This is typically the starting point for determining
     * eligibility. We start with the max eligibility and downgrade it when some constraint is not
     * satisfied.
     */
    static inline BonsaiEligibility fullyEligible() {
        return {FullyEligible};
    }

    bool isFullyEligible() const {
        return _eligibility >= FullyEligible;
    }

    bool isExperimentallyEligible() const {
        return _eligibility >= Experimental;
    }

    /**
     * Update this eligibility to "Ineligible" if the boolean value is true.
     */
    auto setIneligibleIf(bool b) {
        _eligibility = b ? Ineligible : _eligibility;
        return *this;
    }

    auto setIneligible() {
        _eligibility = Ineligible;
        return *this;
    }

    /**
     * Update this eligibility to represent the lower eligibility level between `this` and `other`.
     */
    auto minOf(BonsaiEligibility other) {
        _eligibility = std::min(_eligibility, other._eligibility);
        return *this;
    }

    /**
     * Update this eligibility to represent the lower eligibility level of `this` and another
     * eligibility computed by `f`. If this instance's eligibility already represents the
     * _minEligibility class, `f` will not be evaluated.
     */
    auto minOf(std::function<BonsaiEligibility()> f) {
        if (_eligibility < _minEligibility) {
            return *this;
        }
        this->minOf(f());
        return *this;
    }

private:
    Eligibility _eligibility;

    /**
     * Minimum eligibility required. This allows avoiding redundant constraint checks once this
     * minimum cannot be maintained.
     */
    const Eligibility _minEligibility;
};

namespace optimizer {
/**
 * Visitor that is responsible for indicating whether a DocumentSource is eligible for Bonsai by
 * setting the '_eligibility' member variable. Stages which are "test-only" and not
 * officially supported should set _eligibility to Ineligible.
 */
struct ABTUnsupportedDocumentSourceVisitorContext : public DocumentSourceVisitorContextBase {
    ABTUnsupportedDocumentSourceVisitorContext(bool hasNaturalHint)
        : eligibility(BonsaiEligibility::fullyEligible()), queryHasNaturalHint(hasNaturalHint) {}
    BonsaiEligibility eligibility;
    const bool queryHasNaturalHint;
};
}  // namespace optimizer

template <typename T>
void coutPrintAttr(const logv2::detail::NamedArg<T>& arg) {
    std::cout << arg.name << " : " << arg.value << "\n";
}

template <typename T, typename... Args>
void coutPrintAttr(const logv2::detail::NamedArg<T>& arg,
                   const logv2::detail::NamedArg<Args>&... args) {
    std::cout << arg.name << " : " << arg.value << "\n";
    coutPrintAttr(args...);
}

template <typename... Args>
void coutPrint(const std::string& msg, const logv2::detail::NamedArg<Args>&... args) {
    std::cout << "********* " << msg << " *********\n";
    coutPrintAttr(args...);
    std::cout << "********* " << msg << " *********\n";
}

#define OPTIMIZER_DEBUG_LOG(ID, DLEVEL, FMTSTR_MESSAGE, ...) \
    LOGV2_DEBUG(ID, DLEVEL, FMTSTR_MESSAGE, ##__VA_ARGS__);  \
    if (internalCascadesOptimizerStdCoutDebugOutput.load())  \
        ::mongo::coutPrint(FMTSTR_MESSAGE, __VA_ARGS__);

/**
 * These functions are exposed only for testing; they only perform checks against the query
 * structure. Other callers should use the functions above, which check command and collection
 * options for further details.
 */
BonsaiEligibility isEligibleForBonsai_forTesting(const CanonicalQuery& cq);
BonsaiEligibility isEligibleForBonsai_forTesting(ServiceContext* serviceCtx,
                                                 const Pipeline& pipeline);

bool isBonsaiEnabled(OperationContext* opCtx);

}  // namespace mongo
#undef MONGO_LOGV2_DEFAULT_COMPONENT
