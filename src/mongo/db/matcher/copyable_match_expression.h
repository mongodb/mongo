// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Classes that must be copyable but want to own a MatchExpression (which deletes its copy
 * constructor) can instead store a CopyableMatchExpression.
 *
 * CopyableMatchExpression stores the BSON expression used to created its MatchExpression, so that
 * when we want to copy it, we can create a new MatchExpression that is identical to the old one. We
 * only actually perform this operation, however, when the client wants to mutate the
 * MatchExpression (by calling setCollator()). The rest of the time, copies of a
 * CopyableMatchExpression all point to the same immutable MatchExpression. This pattern is similar
 * to copy-on-write semantics.
 */
class CopyableMatchExpression {
public:
    /**
     * Parse 'matchAST' to create a new MatchExpression, throwing a AssertionException if we
     * encounter an error.
     */
    CopyableMatchExpression(BSONObj matchAST,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            std::unique_ptr<const ExtensionsCallback> extensionsCallback =
                                std::make_unique<ExtensionsCallbackNoop>(),
                            MatchExpressionParser::AllowedFeatureSet allowedFeatures =
                                MatchExpressionParser::kDefaultSpecialFeatures,
                            bool optimizeExpression = false)
        : _matchAST(matchAST), _extensionsCallback(std::move(extensionsCallback)) {
        StatusWithMatchExpression parseResult =
            MatchExpressionParser::parse(_matchAST, expCtx, *_extensionsCallback, allowedFeatures);
        uassertStatusOK(parseResult.getStatus());
        _matchExpr = optimizeExpression ? optimizeMatchExpression(std::move(parseResult.getValue()),
                                                                  /* enableSimplification */ true)
                                        : std::move(parseResult.getValue());
    }

    CopyableMatchExpression(BSONObj matchAST, std::unique_ptr<MatchExpression> matchExpr)
        : _matchAST(matchAST), _matchExpr(std::move(matchExpr)) {}

    /**
     * Sets the collator on the underlying MatchExpression and all clones(!).
     */
    void setCollator(const CollatorInterface* collator) {
        _matchExpr->setCollator(collator);
    }

    /**
     * Overload * so that CopyableMatchExpression can be dereferenced as if it were a pointer to the
     * underlying MatchExpression.
     */
    const MatchExpression& operator*() const {
        return *_matchExpr;
    }

    /**
     * Overload -> so that CopyableMatchExpression can be dereferenced as if it were a pointer to
     * the underlying MatchExpression.
     */
    const MatchExpression* operator->() const {
        return &(*_matchExpr);
    }

    auto inputBSON() const {
        return _matchAST;
    }

private:
    BSONObj _matchAST;
    std::shared_ptr<const ExtensionsCallback> _extensionsCallback;
    std::shared_ptr<MatchExpression> _matchExpr;
};

}  // namespace mongo
