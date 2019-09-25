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

#include <list>
#include <stack>
#include <string>

#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_ast_visitor.h"

namespace mongo {
namespace projection_ast {
using PathTrackingDummyDefaultType = void*;

/**
 * Class for storing context across calls to visit() in the PathTracking visitors.
 *
 * UserData is data that is made available to classes which inherit from the PathTracking visitors.
 */
template <class UserData = PathTrackingDummyDefaultType>
class PathTrackingVisitorContext {
public:
    FieldPath fullPath() const {
        invariant(!_fieldNames.empty());
        invariant(!_fieldNames.top().empty());

        if (!_basePath) {
            return FieldPath(_fieldNames.top().front());
        }
        return FieldPath(
            FieldPath::getFullyQualifiedPath(_basePath->fullPath(), _fieldNames.top().front()));
    }

    const boost::optional<FieldPath>& basePath() const {
        return _basePath;
    }

    void setBasePath(boost::optional<FieldPath> path) {
        _basePath = std::move(path);
    }

    void popFrontFieldName() {
        _fieldNames.top().pop_front();
    }

    void popFieldNames() {
        invariant(_fieldNames.top().empty());
        _fieldNames.pop();
    }

    void pushFieldNames(std::list<std::string> fields) {
        _fieldNames.push(std::move(fields));
    }

    UserData& data() {
        return _data;
    }

private:
    UserData _data;

    // Stores the field names to visit at each layer. Top of the stack is the most recently visited
    // layer.
    std::stack<std::list<std::string>> _fieldNames;

    // All but the last path component of the current path being visited. None if at the top-level
    // and there is no "parent" path.
    boost::optional<FieldPath> _basePath;
};

/**
 * Base visitor used for maintaining field names while traversing the AST.
 *
 * This is intended to be used with the projection AST walker (projection_ast::walk()). Users of
 * this class MUST use both the PathTrackingPreVisitor and PathTrackingPostVisitor in order to
 * correctly maintain the state about the current path being visited.

 * Derived classes can have custom behavior through doVisit() methods on each node type.
 */
template <class Derived, class UserData = PathTrackingDummyDefaultType>
class PathTrackingPreVisitor : public ProjectionASTVisitor {
public:
    PathTrackingPreVisitor(PathTrackingVisitorContext<UserData>* ctx) : _context(ctx) {}

    void visit(MatchExpressionASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ProjectionPathASTNode* node) override {
        if (node->parent()) {
            _context->setBasePath(_context->fullPath());
            _context->popFrontFieldName();
        }

        _context->pushFieldNames({node->fieldNames().begin(), node->fieldNames().end()});

        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ProjectionPositionalASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ProjectionSliceASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ProjectionElemMatchASTNode* node) {
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ExpressionASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(BooleanConstantASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);
    }

protected:
    const FieldPath fullPath() const {
        return _context->fullPath();
    }

    UserData* data() const {
        return _context->data;
    }

private:
    PathTrackingVisitorContext<UserData>* _context;
};

/**
 * Base post-visitor which helps maintain field names while traversing the AST.
 */
template <class Derived, class UserData = PathTrackingDummyDefaultType>
class PathTrackingPostVisitor : public ProjectionASTVisitor {
public:
    PathTrackingPostVisitor(PathTrackingVisitorContext<UserData>* context) : _context(context) {}

    void visit(MatchExpressionASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(projection_ast::ProjectionPathASTNode* node) override {
        static_cast<Derived*>(this)->doVisit(node);

        _context->popFieldNames();

        if (_context->basePath()) {
            // Update the context variable tracking the current path being traversed.
            const FieldPath& fp = *_context->basePath();
            if (fp.getPathLength() == 1) {
                _context->setBasePath(boost::none);
            } else {
                // Pop the last path element.
                _context->setBasePath(FieldPath(fp.withoutLastElement()));
            }
        }
    }

    void visit(ProjectionPositionalASTNode* node) override {
        _context->popFrontFieldName();

        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ProjectionSliceASTNode* node) override {
        _context->popFrontFieldName();

        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ProjectionElemMatchASTNode* node) override {
        _context->popFrontFieldName();

        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(ExpressionASTNode* node) override {
        _context->popFrontFieldName();
        static_cast<Derived*>(this)->doVisit(node);
    }

    void visit(BooleanConstantASTNode* node) override {
        _context->popFrontFieldName();

        static_cast<Derived*>(this)->doVisit(node);
    }

private:
    PathTrackingVisitorContext<UserData>* _context;
};
}  // namespace projection_ast
}  // namespace mongo
