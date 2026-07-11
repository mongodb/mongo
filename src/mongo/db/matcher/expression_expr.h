// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/rewrites/matcher/rewrite_expr.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * MatchExpression for the top-level $expr keyword. Take an expression as an argument, evaluates and
 * coerces to boolean form which determines whether a document is a match.
 */
class ExprMatchExpression final : public MatchExpression {
public:
    ExprMatchExpression(BSONElement elem,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> clone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << "$expr " << _expression->serialize().toString();
        _debugStringAttachTagInfo(&debug);
    }

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    bool isTriviallyTrue() const final;

    bool isTriviallyFalse() const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400207);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    }

    const boost::optional<RewriteExpr::RewriteResult>& getRewriteResult() const {
        return _rewriteResult;
    }

    boost::optional<RewriteExpr::RewriteResult>& getRewriteResult() {
        return _rewriteResult;
    }

    void setRewriteResult(boost::optional<RewriteExpr::RewriteResult> result) {
        _rewriteResult = std::move(result);
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    boost::intrusive_ptr<ExpressionContext> getExpressionContext() const {
        return _expCtx;
    }

    boost::intrusive_ptr<Expression> getExpression() const {
        return _expression;
    }

    /**
     * Use if the caller needs to modify the expression held by this $expr.
     */
    boost::intrusive_ptr<Expression>& getExpressionRef() {
        return _expression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the
     * expression. Each pair in 'renameList' specifies a path prefix that should be renamed (as the
     * first element) and the path components that should replace the renamed prefix (as the second
     * element).
     */
    void applyRename(const StringMap<std::string>& renameList) {
        SubstituteFieldPathWalker substituteWalker(renameList);
        if (auto newExpr =
                expression_walker::walk<Expression>(_expression.get(), &substituteWalker);
            newExpr) {
            _expression = newExpr.release();
        }
    }

    bool hasRenameablePath(const StringMap<std::string>& renameList) const {
        bool hasRenameablePath = false;
        FieldPathVisitor visitor([&](const ExpressionFieldPath* expr) {
            hasRenameablePath =
                hasRenameablePath || expr->isRenameableByAnyPrefixNameIn(renameList);
        });
        stage_builder::ExpressionWalker walker(
            &visitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
        expression_walker::walk(_expression.get(), &walker);
        return hasRenameablePath;
    }

    std::string_view path() const final {
        if (_path) {
            return std::string_view(_path.get());
        }
        return std::string_view();
    }

    /**
     * If the expression is an equality to not-null constant, return a BSONElement of the form
     * {<path> : <constant>}. This is a help function to build index bounds for this special kind of
     * expression.
     */
    boost::optional<BSONElement> getData() const;

private:
    void _doSetCollator(const CollatorInterface* collator) final;

    void setPathForEquality(boost::intrusive_ptr<Expression> expression);

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    boost::intrusive_ptr<Expression> _expression;

    boost::optional<RewriteExpr::RewriteResult> _rewriteResult;

    boost::optional<std::string> _path;

    // Internal storage for the equivalent $eq expression.
    boost::optional<BSONObj> _backingBSON;
};

}  // namespace mongo
