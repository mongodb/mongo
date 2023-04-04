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

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"

namespace mongo::fle {
/**
 * Class which handles traversing expressions and rewriting predicates for FLE2.
 *
 * The QueryRewriter is responsible for traversing Agg Expressions and MatchExpression trees and
 * calling individual rewrites (subclasses of EncryptedPredicate) that have been registered for each
 * encrypted index type.
 *
 * The actual rewrites performed are stored in references to maps in the class. In non-test
 * environments, these are global maps that register encrypted predicate rewrites that live in their
 * own files.
 */
class QueryRewriter : public QueryRewriterInterface {
public:
    /**
     * Takes in references to collection readers for the ESC that are used during tag
     * computation.
     */
    QueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx,
                  FLETagQueryInterface* tagQueryInterface,
                  const NamespaceString& nssEsc,
                  EncryptedCollScanModeAllowed mode = EncryptedCollScanModeAllowed::kAllow)
        : _expCtx(expCtx),
          _exprRewrites(aggPredicateRewriteMap),
          _matchRewrites(matchPredicateRewriteMap),
          _nssEsc(nssEsc),
          _tagQueryInterface(tagQueryInterface) {

        if (internalQueryFLEAlwaysUseEncryptedCollScanMode.load()) {
            _mode = EncryptedCollScanMode::kForceAlways;
        }

        if (mode == EncryptedCollScanModeAllowed::kDisallow) {
            _mode = EncryptedCollScanMode::kDisallow;
        }

        // This isn't the "real" query so we don't want to increment Expression
        // counters here.
        _expCtx->stopExpressionCounters();
    }

    /**
     * Accepts a BSONObj holding a MatchExpression, and returns BSON representing the rewritten
     * expression. Returns boost::none if no rewriting was done.
     *
     * Rewrites the match expression with FLE find payloads into a disjunction on the
     * __safeContent__ array of tags.
     *
     * Will rewrite top-level $eq and $in expressions, as well as recursing through $and, $or, $not
     * and $nor. Also handles similarly limited rewriting under $expr. All other MatchExpressions,
     * notably $elemMatch, are ignored.
     */
    boost::optional<BSONObj> rewriteMatchExpression(const BSONObj& filter);

    /**
     * Accepts an expression to be re-written. Will rewrite top-level expressions including $eq and
     * $in, as well as recursing through other expressions. Returns a new pointer if the top-level
     * expression must be changed. A nullptr indicates that the modifications happened in-place.
     */
    std::unique_ptr<Expression> rewriteExpression(Expression* expression);

    bool isForceEncryptedCollScan() const {
        return _mode == EncryptedCollScanMode::kForceAlways;
    }

    void setForceEncryptedCollScanForTest() {
        _mode = EncryptedCollScanMode::kForceAlways;
    }

    EncryptedCollScanMode getEncryptedCollScanMode() const override {
        return _mode;
    }

    ExpressionContext* getExpressionContext() const override {
        return _expCtx.get();
    }

    FLETagQueryInterface* getTagQueryInterface() const override {
        return _tagQueryInterface;
    }

    const NamespaceString& getESCNss() const override {
        return _nssEsc;
    }

protected:
    // This constructor should only be used for mocks in testing.
    QueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx,
                  const NamespaceString& nssEsc,
                  const ExpressionToRewriteMap& exprRewrites,
                  const MatchTypeToRewriteMap& matchRewrites)
        : _expCtx(expCtx),
          _exprRewrites(exprRewrites),
          _matchRewrites(matchRewrites),
          _nssEsc(nssEsc),
          _tagQueryInterface(nullptr) {}

private:
    /**
     * A single rewrite step, called recursively on child expressions.
     */
    std::unique_ptr<MatchExpression> _rewrite(MatchExpression* me);

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // True if the last Expression or MatchExpression processed by this rewriter was rewritten.
    bool _rewroteLastExpression = false;

    // Controls how query rewriter rewrites the query
    EncryptedCollScanMode _mode{EncryptedCollScanMode::kUseIfNeeded};

    const ExpressionToRewriteMap& _exprRewrites;
    const MatchTypeToRewriteMap& _matchRewrites;
    const NamespaceString& _nssEsc;
    FLETagQueryInterface* _tagQueryInterface;
};
}  // namespace mongo::fle
