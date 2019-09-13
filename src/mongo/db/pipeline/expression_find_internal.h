/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <fmt/format.h>

#include "mongo/db/exec/find_projection_executor.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {
/**
 * An internal expression to apply a find positional projection to the projection post image
 * document. This expression is never parsed or serialized.
 *
 * See projection_executor::applyPositionalProjection() for more details.
 */
class ExpressionInternalFindPositional final : public Expression {
public:
    ExpressionInternalFindPositional(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     boost::intrusive_ptr<Expression> child,
                                     FieldPath path,
                                     const MatchExpression* matchExpr)
        : Expression{expCtx, {child}}, _path{std::move(path)}, _matchExpr{matchExpr} {}

    Value evaluate(const Document& root, Variables* variables) const final {
        using namespace fmt::literals;

        auto postImage = _children[0]->evaluate(root, variables);
        uassert(51255,
                "Positional operator can only be applied to an object, but got {}"_format(
                    typeName(postImage.getType())),
                postImage.getType() == BSONType::Object);
        return Value{projection_executor::applyPositionalProjection(
            postImage.getDocument(), *_matchExpr, _path)};
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    Value serialize(bool explain) const final {
        MONGO_UNREACHABLE;
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar = Variables::kRootId) const final {
        // The positional projection can change any field within the path it's applied to, so we'll
        // treat the first component in '_positionalInfo.path' as the computed path.
        return {{_path.front().toString()}, {}};
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        _children[0]->addDependencies(deps);
        deps->needWholeDocument = true;
        _matchExpr->addDependencies(deps);
    }

private:
    const FieldPath _path;
    const MatchExpression* _matchExpr;
};

/**
 * An internal expression to apply a find $slice projection to the projection post image document.
 * This expression is never parsed or serialized.
 *
 * See projection_executor::applySliceProjection() for more details.
 */
class ExpressionInternalFindSlice final : public Expression {
public:
    ExpressionInternalFindSlice(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                boost::intrusive_ptr<Expression> child,
                                FieldPath path,
                                boost::optional<int> skip,
                                int limit)
        : Expression{expCtx, {child}}, _path{std::move(path)}, _skip{skip}, _limit{limit} {}

    Value evaluate(const Document& root, Variables* variables) const final {
        using namespace fmt::literals;

        auto postImage = _children[0]->evaluate(root, variables);
        uassert(51256,
                "$slice operator can only be applied to an object, but got {}"_format(
                    typeName(postImage.getType())),
                postImage.getType() == BSONType::Object);
        return Value{projection_executor::applySliceProjection(
            postImage.getDocument(), _path, _skip, _limit)};
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    Value serialize(bool explain) const final {
        MONGO_UNREACHABLE;
    }

    ComputedPaths getComputedPaths(const std::string& exprFieldPath,
                                   Variables::Id renamingVar = Variables::kRootId) const final {
        // The $slice projection can change any field within the path it's applied to, so we'll
        // treat the first component in '_sliceInfo.path' as the computed path.
        return {{_path.front().toString()}, {}};
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        _children[0]->addDependencies(deps);
        deps->needWholeDocument = true;
    }

private:
    const FieldPath _path;
    const boost::optional<int> _skip;
    const int _limit;
};

/**
 * An internal expression to apply a find $elemMatch projection to the document root.
 * See projection_executor::applyElemMatchProjection() for details.
 */
class ExpressionInternalFindElemMatch final : public Expression {
public:
    ExpressionInternalFindElemMatch(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    boost::intrusive_ptr<Expression> child,
                                    FieldPath path,
                                    std::unique_ptr<MatchExpression> matchExpr)
        : Expression{expCtx, {child}}, _path{std::move(path)}, _matchExpr{std::move(matchExpr)} {}

    Value evaluate(const Document& root, Variables* variables) const final {
        using namespace fmt::literals;

        auto input = _children[0]->evaluate(root, variables);
        uassert(51253,
                "$elemMatch operator can only be applied to an object, but got {}"_format(
                    typeName(input.getType())),
                input.getType() == BSONType::Object);
        return projection_executor::applyElemMatchProjection(
            input.getDocument(), *_matchExpr, _path);
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    Value serialize(bool explain) const final {
        MONGO_UNREACHABLE;
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        _children[0]->addDependencies(deps);
        deps->needWholeDocument = true;
        _matchExpr->addDependencies(deps);
    }

private:
    const FieldPath _path;
    const std::unique_ptr<MatchExpression> _matchExpr;
};
}  // namespace mongo
