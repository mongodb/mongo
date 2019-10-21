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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/projection_ast_visitor.h"

namespace mongo {
namespace projection_ast {
/*
 * A tree representation of a projection. The main purpose of this class is to offer a typed,
 * walkable representation of a projection. It's mostly meant to be used while doing validation and
 * dependency analysis. It is not designed for executing a projection.
 */
class ASTNode {
public:
    using ASTNodeVector = std::vector<std::unique_ptr<ASTNode>>;

    ASTNode() {}
    ASTNode(ASTNodeVector children) : _children(std::move(children)) {
        for (auto&& child : _children) {
            child->_parent = this;
        }
    }

    ASTNode(const ASTNode& other) : _parent(nullptr) {
        // It is the responsibility of this node's parent to set _parent on this node correctly.
        _children.reserve(other._children.size());
        for (auto&& child : other._children) {
            addChildToInternalVector(child->clone());
        }
    }
    ASTNode(ASTNode&&) = default;

    virtual ~ASTNode() = default;

    virtual std::unique_ptr<ASTNode> clone() const = 0;

    virtual void acceptVisitor(ProjectionASTMutableVisitor* visitor) = 0;
    virtual void acceptVisitor(ProjectionASTConstVisitor* visitor) const = 0;

    const ASTNodeVector& children() const {
        return _children;
    }

    ASTNode* child(size_t index) const {
        invariant(index < _children.size());
        return _children[index].get();
    }

    const ASTNode* parent() const {
        return _parent;
    }

    bool isRoot() const {
        return !_parent;
    }

protected:
    virtual void addChildToInternalVector(std::unique_ptr<ASTNode> node) {
        node->_parent = this;
        _children.push_back(std::move(node));
    }

    // nullptr if this is the root.
    ASTNode* _parent = nullptr;
    ASTNodeVector _children;
};

class MatchExpressionASTNode final : public ASTNode {
public:
    MatchExpressionASTNode(CopyableMatchExpression matchExpr) : _matchExpr{matchExpr} {}

    MatchExpressionASTNode(const MatchExpressionASTNode& other)
        : ASTNode{other}, _matchExpr{other._matchExpr} {}

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<MatchExpressionASTNode>(*this);
    }

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    CopyableMatchExpression matchExpression() const {
        return _matchExpr;
    }

private:
    CopyableMatchExpression _matchExpr;
};

class ProjectionPathASTNode final : public ASTNode {
public:
    ProjectionPathASTNode() {}

    ProjectionPathASTNode(ASTNodeVector children, std::vector<std::string> fieldNames)
        : ASTNode(std::move(children)), _fieldNames(std::move(fieldNames)) {
        invariant(_children.size() == _fieldNames.size());
    }

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<ProjectionPathASTNode>(*this);
    }

    ASTNode* getChild(StringData fieldName) const {
        invariant(_fieldNames.size() == _children.size());
        for (size_t i = 0; i < _fieldNames.size(); ++i) {
            if (_fieldNames[i] == fieldName) {
                return _children[i].get();
            }
        }
        return nullptr;
    }

    void addChild(StringData fieldName, std::unique_ptr<ASTNode> node) {
        addChildToInternalVector(std::move(node));
        _fieldNames.push_back(fieldName.toString());
    }

    /**
     * Remove a node which is a direct child of this tree. Returns true if anything was removed,
     * false otherwise.
     */
    bool removeChild(StringData fieldName) {
        if (auto it = std::find(_fieldNames.begin(), _fieldNames.end(), fieldName);
            it != _fieldNames.end()) {
            _children.erase(_children.begin() + std::distance(_fieldNames.begin(), it));
            _fieldNames.erase(it);
            return true;
        }

        return false;
    }

    const std::vector<std::string>& fieldNames() const {
        return _fieldNames;
    }

private:
    // Names associated with the child nodes. Must be same size as _children.
    std::vector<std::string> _fieldNames;
};

class ProjectionPositionalASTNode final : public ASTNode {
public:
    ProjectionPositionalASTNode(std::unique_ptr<MatchExpressionASTNode> child) {
        invariant(child);
        addChildToInternalVector(std::move(child));
    }

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<ProjectionPositionalASTNode>(*this);
    }
};

class ProjectionSliceASTNode final : public ASTNode {
public:
    ProjectionSliceASTNode(boost::optional<int> skip, int limit) : _skip(skip), _limit(limit) {}

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<ProjectionSliceASTNode>(*this);
    }

    int limit() const {
        return _limit;
    }

    boost::optional<int> skip() const {
        return _skip;
    }

private:
    boost::optional<int> _skip;
    int _limit = 0;
};

class ProjectionElemMatchASTNode final : public ASTNode {
public:
    ProjectionElemMatchASTNode(std::unique_ptr<MatchExpressionASTNode> child) {
        invariant(child);
        addChildToInternalVector(std::move(child));
    }

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<ProjectionElemMatchASTNode>(*this);
    }
};

class ExpressionASTNode final : public ASTNode {
public:
    ExpressionASTNode(boost::intrusive_ptr<Expression> expr) : _expr(expr) {}
    ExpressionASTNode(const ExpressionASTNode& other) : ASTNode(other) {
        BSONObjBuilder bob;
        bob << "" << other._expr->serialize(false);

        // TODO SERVER-31003: add a clone() method to Expression.
        boost::intrusive_ptr<Expression> clonedExpr =
            Expression::parseOperand(other._expr->getExpressionContext(),
                                     bob.obj().firstElement(),
                                     other._expr->getExpressionContext()->variablesParseState);
        _expr = clonedExpr;
    }

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<ExpressionASTNode>(*this);
    }

    Expression* expressionRaw() const {
        return _expr.get();
    }

    boost::intrusive_ptr<Expression> expression() const {
        return _expr;
    }

private:
    boost::intrusive_ptr<Expression> _expr;
};

class BooleanConstantASTNode final : public ASTNode {
public:
    BooleanConstantASTNode(bool val) : _val(val) {}

    void acceptVisitor(ProjectionASTMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    void acceptVisitor(ProjectionASTConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    std::unique_ptr<ASTNode> clone() const override final {
        return std::make_unique<BooleanConstantASTNode>(*this);
    }

    bool value() const {
        return _val;
    }

private:
    bool _val;
};
}  // namespace projection_ast
}  // namespace mongo
