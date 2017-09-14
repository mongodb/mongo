/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"

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
                                stdx::make_unique<ExtensionsCallbackNoop>(),
                            MatchExpressionParser::AllowedFeatureSet allowedFeatures =
                                MatchExpressionParser::kDefaultSpecialFeatures)
        : _matchAST(matchAST), _extensionsCallback(std::move(extensionsCallback)) {
        StatusWithMatchExpression parseResult =
            MatchExpressionParser::parse(_matchAST, expCtx, *_extensionsCallback, allowedFeatures);
        uassertStatusOK(parseResult.getStatus());
        _matchExpr = std::move(parseResult.getValue());
    }

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
    const MatchExpression& operator*() {
        return *_matchExpr;
    }

    /**
     * Overload -> so that CopyableMatchExpression can be dereferenced as if it were a pointer to
     * the underlying MatchExpression.
     */
    const MatchExpression* operator->() {
        return &(*_matchExpr);
    }

private:
    BSONObj _matchAST;
    std::shared_ptr<const ExtensionsCallback> _extensionsCallback;
    std::shared_ptr<MatchExpression> _matchExpr;
};

}  // namespace mongo
