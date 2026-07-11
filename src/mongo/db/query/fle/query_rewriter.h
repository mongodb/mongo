// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/query/fle/text_search_predicate.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
     * computation. Delegates to protected constructor with the appropriate EncryptedCollScanMode.
     */
    QueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx,
                  FLETagQueryInterface* tagQueryInterface,
                  const NamespaceString& nssEsc,
                  const std::map<NamespaceString, EncryptedFieldConfig>& efcMap,
                  EncryptedCollScanModeAllowed mode = EncryptedCollScanModeAllowed::kAllow)
        : QueryRewriter(std::move(expCtx),
                        tagQueryInterface,
                        nssEsc,
                        aggPredicateRewriteMap,
                        matchPredicateRewriteMap,
                        efcMap,
                        [&]() {
                            EncryptedCollScanMode modeResult{EncryptedCollScanMode::kUseIfNeeded};

                            if (internalQueryFLEAlwaysUseEncryptedCollScanMode.load()) {
                                modeResult = EncryptedCollScanMode::kForceAlways;
                            }

                            if (mode == EncryptedCollScanModeAllowed::kDisallow) {
                                modeResult = EncryptedCollScanMode::kDisallow;
                            }
                            return modeResult;
                        }()) {}

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

    // Enables find-payload validation. Caller owns `efc`.
    void setEncryptedFieldConfigForValidation(const EncryptedFieldConfig* efc) {
        _efc = efc;
    }

    boost::optional<const EncryptedFieldConfig&> getEncryptedFieldConfigForValidation()
        const override {
        if (_efc) {
            return *_efc;
        }
        return boost::none;
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
        uassert(
            10026006, "Invalid request of escNss for unencrypted collection", !_nssEsc.isEmpty());
        return _nssEsc;
    }
    /**
     * createSubpipelineRewriter is used in the context of PipelineRewrite. When a sub-pipeline must
     * be rewritten (i.e $lookup), this method uses its map of collection Ns to ESC metadata
     * collection name to create a new QueryRewriter for the sub-pipeline.
     */
    QueryRewriter createSubpipelineRewriter(
        const NamespaceString& collectionNss,
        boost::intrusive_ptr<ExpressionContext> subpipelineExpCtx) {
        tassert(9775501,
                "createSubpipelineRewriter is not supported when feature flag is disabled",
                feature_flags::gFeatureFlagLookupEncryptionSchemasFLE.isEnabled());

        tassert(9775506, "Invalid subpipeline expression context", subpipelineExpCtx);
        const auto iter = _efcMap.find(collectionNss);
        auto buildSub = [&](const NamespaceString& subEsc) {
            QueryRewriter sub(std::move(subpipelineExpCtx),
                              _tagQueryInterface,
                              subEsc,
                              _exprRewrites,
                              _matchRewrites,
                              _efcMap,
                              _mode);
            if (iter != _efcMap.end()) {
                sub.setEncryptedFieldConfigForValidation(&iter->second);
            }
            return sub;
        };
        if (iter == _efcMap.end()) {
            /**
             * If we couldn't find an entry in the _efcMap, we are rewriting a pipeline which
             * involves both QE collections and unencrypted collections. In this case, we provide an
             * empty namespace string, which will lead to an error if we try to request for the esc
             * collection for the sub-pipeline when rewriting to the tag disjunction. In the
             * unlikely event that we try to rewrite to a runtime comparison, there will be no
             * error, but the query expression in question won't be rewritten, which is the intended
             * behavior.
             */
            return buildSub(NamespaceString());
        }
        auto subEsc = NamespaceStringUtil::deserialize(
            collectionNss.dbName(), std::string{*iter->second.getEscCollection()});
        uassert(10026007, "Unexpected empty nssEsc for QE schema", !subEsc.isEmpty());
        return buildSub(subEsc);
    }

protected:
    // Constructor that gets delegated to. This constructor is required so that
    // createSubpipelineRewriter() can pass its exprRewrites and matchRewrites to the sub-pipeline's
    // rewriter.
    QueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx,
                  FLETagQueryInterface* tagQueryInterface,
                  const NamespaceString& nssEsc,
                  const ExpressionToRewriteMap& exprRewrites,
                  const MatchTypeToRewriteMap& matchRewrites,
                  const std::map<NamespaceString, EncryptedFieldConfig>& efcMap,
                  EncryptedCollScanMode mode)
        : _expCtx(std::move(expCtx)),
          _mode(mode),
          _exprRewrites(exprRewrites),
          _matchRewrites(matchRewrites),
          _nssEsc(nssEsc),
          _tagQueryInterface(tagQueryInterface),
          _efcMap(efcMap) {

        // This isn't the "real" query so we don't want to increment Expression
        // counters here.
        _expCtx->stopExpressionCounters();
    }

    // This constructor should only be used for mocks in testing.
    QueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx,
                  const NamespaceString& nssEsc,
                  const ExpressionToRewriteMap& exprRewrites,
                  const MatchTypeToRewriteMap& matchRewrites,
                  const std::map<NamespaceString, EncryptedFieldConfig>& efcMap)
        : _expCtx(std::move(expCtx)),
          _exprRewrites(exprRewrites),
          _matchRewrites(matchRewrites),
          _nssEsc(nssEsc),
          _tagQueryInterface(nullptr),
          _efcMap(efcMap) {}

private:
    /**
     * A single rewrite step, called recursively on child expressions.
     */
    std::unique_ptr<MatchExpression> _rewrite(MatchExpression* me);

    /**
     * Performs FLE rewrites of an aggregation expression under a match $expr, returns a new $expr
     * wrapping the rewritten expression if a top level rewrite took place. Otherwise returns
     * nullptr.
     * This method updates _rewroteLastExpression if a rewrite took place which did not generate a
     * top level rewrite, which ensures that we serialize the expression in rewriteMatchExpression
     * to reflect the nested rewrites.
     */
    std::unique_ptr<MatchExpression> _rewriteExprMatchExpression(
        ExprMatchExpression& exprMatchExpression);

    /**
     * We provide friendship to EncryptedTextSearchExpressionDetector so that it can access
     * _getEncryptedTextSearchPredicate without having to expose this method as part of the public
     * interface.
     */
    friend class EncryptedTextSearchExpressionDetector;

    /**
     * This method is required to facilitate unit testing the specific changes implemented to
     * generate index scans for encrypted text search predicates. This requires that we expose the
     * TextSearchPredicate name in this header. If we find this to be a problem, we can fix this by
     * implementing our changes in the EncryptedPredicate interface instead, but that requires more
     * plumbing changes.
     *
     * NOTE: It is only valid to call this method if _hasValidTextSearchPredicate() is true.
     */
    const TextSearchPredicate& _getEncryptedTextSearchPredicate() const;
    bool _hasValidTextSearchPredicate() const;
    bool _initializeTextSearchPredicate();

    // _initializeTextSearchPredicateInternal() is only called from _initializeTextSearchPredicate,
    // which guarantees that we never re-initialize our predicate.
    virtual std::unique_ptr<TextSearchPredicate> _initializeTextSearchPredicateInternal() const;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // True if the last Expression or MatchExpression processed by this rewriter was rewritten.
    bool _rewroteLastExpression = false;

    // Controls how query rewriter rewrites the query
    EncryptedCollScanMode _mode{EncryptedCollScanMode::kUseIfNeeded};

    const ExpressionToRewriteMap& _exprRewrites;
    const MatchTypeToRewriteMap& _matchRewrites;
    const NamespaceString _nssEsc;
    FLETagQueryInterface* _tagQueryInterface;
    // Map of collection Ns to its EncryptedFieldConfig.
    // Owned by caller. Lifetime must always exceed QueryRewriter.
    const std::map<NamespaceString, EncryptedFieldConfig>& _efcMap;
    std::unique_ptr<TextSearchPredicate> _textSearchPredicate;
    const EncryptedFieldConfig* _efc = nullptr;
};
}  // namespace mongo::fle
