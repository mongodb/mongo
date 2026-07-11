// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class is used by the aggregation framework and streams enterprise module to perform the
 * document processing needed for $match.
 */
class [[MONGO_MOD_PUBLIC]] MatchProcessor {
public:
    MatchProcessor(std::unique_ptr<MatchExpression> expr,
                   DepsTracker dependencies,
                   BSONObj&& predicate);

    // Processes the given document and returns true if it matches the conditions specified in
    // the MatchExpression. The optional 'ctx' parameter carries evaluation state (see
    // EvaluationContext); when it holds a memory tracker, memory usage observed while evaluating
    // any $expr sub-expressions within the match expression is accumulated against it.
    bool process(const Document& input, const EvaluationContext& ctx = {}) const;

    std::unique_ptr<MatchExpression>& getExpression() {
        return _expression;
    }

    const std::unique_ptr<MatchExpression>& getExpression() const {
        return _expression;
    }

    void setExpression(std::unique_ptr<MatchExpression> expression) {
        _expression = std::move(expression);
    }

    const BSONObj& getPredicate() const {
        return _predicate;
    }

private:
    // Determines whether all paths have unique first fields. This is called once during object
    // construction to determine the value of '_dependenciesHaveUniqueFirstFields'.
    static bool dependenciesHaveUniqueFirstFields(const OrderedPathSet& paths);

    std::unique_ptr<MatchExpression> _expression;

    // Cache the dependencies so that we know what fields we need to serialize to BSON for
    // matching.
    DepsTracker _dependencies;

    // Whether or not the paths in '_dependencies.fields' have unique first fields or not. Based on
    // the uniqueness check outcome the match processor may be able to use an optimized code path
    // when converting input Documents to BSONObjs.
    const bool _dependenciesHaveUniqueFirstFields;

    // Store the BSONObj that backs this '_expression' so that it doesn't get disposed before the
    // match expression does.
    BSONObj _predicate;
};

}  // namespace mongo
