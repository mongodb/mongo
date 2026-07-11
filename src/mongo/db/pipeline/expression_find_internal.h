// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>

namespace mongo {
/**
 * An internal expression to apply a find positional projection to the projection post image
 * document. This expression is never parsed or serialized.
 *
 * See projection_executor::applyPositionalProjection() for more details.
 */
class ExpressionInternalFindPositional final : public Expression {
public:
    ExpressionInternalFindPositional(ExpressionContext* const expCtx,
                                     boost::intrusive_ptr<Expression> preImageExpr,
                                     boost::intrusive_ptr<Expression> postImageExpr,
                                     FieldPath path,
                                     CopyableMatchExpression matchExpr)
        : Expression{expCtx, {preImageExpr, postImageExpr}},
          _path{std::move(path)},
          _matchExpr{std::move(matchExpr)} {}

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    Value serialize(const query_shape::SerializationOptions& options) const final {
        MONGO_UNREACHABLE;
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar = Variables::kRootId) const final {
        // The positional projection can change any field within the path it's applied to, so we'll
        // treat the first component in '_positionalInfo.path' as the computed path.
        return {{std::string{_path.front()}}, {}};
    }

    boost::intrusive_ptr<Expression> optimize() final {
        for (auto& child : _children) {
            child = child->optimize();
        }
        // SERVER-43740: ideally we'd want to optimize '_matchExpr' here as well. However, given
        // that the match expression is stored as a shared copyable expression in this class, and
        // 'optimizeMatchExpression()' takes and returns a unique pointer on a match expression,
        // there is no easy way to replace a copyable match expression with its optimized
        // equivalent. So, for now we will assume that the copyable match expression is passed to
        // this expression already optimized. Once we have MatchExpression and Expression combined,
        // we should revisit this code and make sure that 'optimized()' method is called on
        // _matchExpr.
        return this;
    }

    const CopyableMatchExpression& getMatchExpr() const {
        return _matchExpr;
    }

    const FieldPath& getFieldPath() const {
        return _path;
    }

    const CopyableMatchExpression& getMatchExpression() const {
        return _matchExpr;
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionInternalFindPositional>(&expCtx,
                                                                cloneChild(_kPreImageExpr, expCtx),
                                                                cloneChild(_kPostImageExpr, expCtx),
                                                                _path,
                                                                _matchExpr);
    }

private:
    static constexpr size_t _kPreImageExpr = 0;
    static constexpr size_t _kPostImageExpr = 1;

    const FieldPath _path;
    const CopyableMatchExpression _matchExpr;
};

/**
 * An internal expression to apply a find $slice projection to the projection post image document.
 * This expression is never parsed or serialized.
 *
 * See projection_executor::applySliceProjection() for more details.
 */
class ExpressionInternalFindSlice final : public Expression {
public:
    ExpressionInternalFindSlice(ExpressionContext* const expCtx,
                                boost::intrusive_ptr<Expression> postImageExpr,
                                FieldPath path,
                                boost::optional<int> skip,
                                int limit)
        : Expression{expCtx, {postImageExpr}}, _path{std::move(path)}, _skip{skip}, _limit{limit} {}

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    Value serialize(const query_shape::SerializationOptions& options) const final {
        MONGO_UNREACHABLE;
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar = Variables::kRootId) const final {
        // The $slice projection can change any field within the path it's applied to, so we'll
        // treat the first component in '_sliceInfo.path' as the computed path.
        return {{std::string{_path.front()}}, {}};
    }

    boost::intrusive_ptr<Expression> optimize() final {
        tassert(11282953,
                str::stream() << "Expect exactly 1 child node in ExpressionInternalFindSlice, got "
                              << _children.size(),
                _children.size() == 1ul);

        _children[0] = _children[0]->optimize();
        return this;
    }

    const FieldPath& getFieldPath() const {
        return _path;
    }

    const boost::optional<int>& getSkip() const {
        return _skip;
    }

    int getLimit() const {
        return _limit;
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionInternalFindSlice>(
            &expCtx, cloneChild(0, expCtx), _path, _skip, _limit);
    }

private:
    const FieldPath _path;
    const boost::optional<int> _skip;
    const int _limit;

    template <typename H>
    friend class ExpressionHashVisitor;
};

/**
 * An internal expression to apply a find $elemMatch projection to the document root.
 * See projection_executor::applyElemMatchProjection() for details.
 */
class ExpressionInternalFindElemMatch final : public Expression {
public:
    ExpressionInternalFindElemMatch(ExpressionContext* const expCtx,
                                    boost::intrusive_ptr<Expression> child,
                                    FieldPath path,
                                    CopyableMatchExpression matchExpr)
        : Expression{expCtx, {child}}, _path{std::move(path)}, _matchExpr{std::move(matchExpr)} {}

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    Value serialize(const query_shape::SerializationOptions& options) const final {
        MONGO_UNREACHABLE;
    }

    boost::intrusive_ptr<Expression> optimize() final {
        tassert(
            11282952,
            str::stream() << "Expect exactly 1 child node in ExpressionInternalFindElemMatch, got "
                          << _children.size(),
            _children.size() == 1ul);

        _children[0] = _children[0]->optimize();
        // SERVER-43740: ideally we'd want to optimize '_matchExpr' here as well. However, given
        // that the match expression is stored as a shared copyable expression in this class, and
        // 'optimizeMatchExpression()' takes and returns a unique pointer on a match expression,
        // there is no easy way to replace a copyable match expression with its optimized
        // equivalent. So, for now we will assume that the copyable match expression is passed to
        // this expression already optimized. Once we have MatchExpression and Expression combined,
        // we should revisit this code and make sure that 'optimized()' method is called on
        // _matchExpr.
        return this;
    }

    const CopyableMatchExpression& getMatchExpr() const {
        return _matchExpr;
    }

    const FieldPath& getFieldPath() const {
        return _path;
    }

    const CopyableMatchExpression& getMatchExpression() const {
        return _matchExpr;
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionInternalFindElemMatch>(
            &expCtx, cloneChild(0, expCtx), _path, _matchExpr);
    }

private:
    const FieldPath _path;
    const CopyableMatchExpression _matchExpr;
};
}  // namespace mongo
