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
    PathTrackingVisitorContext() {}
    PathTrackingVisitorContext(UserData data) : _data{std::move(data)} {}

    auto fullPath() const {
        invariant(!_fieldNames.empty());
        invariant(!_fieldNames.top().empty());

        if (!_basePath) {
            return FieldPath(_fieldNames.top().front());
        }
        return FieldPath(
            FieldPath::getFullyQualifiedPath(_basePath->fullPath(), _fieldNames.top().front()));
    }

    const auto& basePath() const {
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

    auto& data() {
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

namespace {
/**
 * A path tracking pre-visitor used for maintaining field names while traversing the AST.
 *
 * This is intended to be used with the 'ProjectionPathTrackingWalker' only to correctly maintain
 * the state about the current path being visited.
 */
template <class UserData = PathTrackingDummyDefaultType>
class PathTrackingPreVisitor final : public ProjectionASTVisitor {
public:
    PathTrackingPreVisitor(PathTrackingVisitorContext<UserData>* context) : _context{context} {
        invariant(_context);
    }

    void visit(ProjectionPathASTNode* node) final {
        if (node->parent()) {
            _context->setBasePath(_context->fullPath());
            _context->popFrontFieldName();
        }

        _context->pushFieldNames({node->fieldNames().begin(), node->fieldNames().end()});
    }

    void visit(MatchExpressionASTNode* node) final {}
    void visit(ProjectionPositionalASTNode* node) final {}
    void visit(ProjectionSliceASTNode* node) final {}
    void visit(ProjectionElemMatchASTNode* node) final {}
    void visit(ExpressionASTNode* node) final {}
    void visit(BooleanConstantASTNode* node) final {}

private:
    PathTrackingVisitorContext<UserData>* _context;
};

/**
 * A path tracking post-visitor used for maintaining field names while traversing the AST.
 *
 * This is intended to be used with the 'PathTrackingWalker' only to correctly maintain the state
 * about the current path being visited.
 */
template <class UserData = PathTrackingDummyDefaultType>
class PathTrackingPostVisitor final : public ProjectionASTVisitor {
public:
    PathTrackingPostVisitor(PathTrackingVisitorContext<UserData>* context) : _context{context} {
        invariant(_context);
    }

    void visit(projection_ast::ProjectionPathASTNode* node) final {
        _context->popFieldNames();

        if (_context->basePath()) {
            // Update the context variable tracking the current path being traversed.
            const auto& fp = *_context->basePath();
            if (fp.getPathLength() == 1) {
                _context->setBasePath(boost::none);
            } else {
                // Pop the last path element.
                _context->setBasePath(FieldPath(fp.withoutLastElement()));
            }
        }
    }

    void visit(ProjectionPositionalASTNode* node) final {
        _context->popFrontFieldName();
    }

    void visit(ProjectionSliceASTNode* node) final {
        _context->popFrontFieldName();
    }

    void visit(ProjectionElemMatchASTNode* node) final {
        _context->popFrontFieldName();
    }

    void visit(ExpressionASTNode* node) final {
        _context->popFrontFieldName();
    }

    void visit(BooleanConstantASTNode* node) final {
        _context->popFrontFieldName();
    }

    void visit(MatchExpressionASTNode* node) final {}

private:
    PathTrackingVisitorContext<UserData>* _context;
};
}  // namespace

/**
 * A general path tracking walker to be used with projection AST visitors which need to track
 * the current projection path. Visitors will be able to access the current path via
 * 'PathTrackingVisitorContext', passed as 'context' parameter to the constructor of this class.
 * The walker and the visitors must be initialized with the same 'context' pointer.
 *
 * The visitors specified in the 'preVisitors' and 'postVisitors' parameters will be visited in
 * the same order as they were added to the vector.
 */
template <class UserData = PathTrackingDummyDefaultType>
class PathTrackingWalker final {
public:
    PathTrackingWalker(PathTrackingVisitorContext<UserData>* context,
                       std::vector<ProjectionASTVisitor*> preVisitors,
                       std::vector<ProjectionASTVisitor*> postVisitors)
        : _pathTrackingPreVisitor{context},
          _pathTrackingPostVisitor{context},
          _preVisitors{std::move(preVisitors)},
          _postVisitors{std::move(postVisitors)} {
        _preVisitors.insert(_preVisitors.begin(), &_pathTrackingPreVisitor);
        _postVisitors.push_back(&_pathTrackingPostVisitor);
    }

    void preVisit(projection_ast::ASTNode* node) {
        for (auto visitor : _preVisitors) {
            node->acceptVisitor(visitor);
        }
    }

    void postVisit(projection_ast::ASTNode* node) {
        for (auto visitor : _postVisitors) {
            node->acceptVisitor(visitor);
        }
    }

    void inVisit(long count, projection_ast::ASTNode* node) {}

private:
    PathTrackingPreVisitor<UserData> _pathTrackingPreVisitor;
    PathTrackingPostVisitor<UserData> _pathTrackingPostVisitor;
    std::vector<ProjectionASTVisitor*> _preVisitors;
    std::vector<ProjectionASTVisitor*> _postVisitors;
};
}  // namespace projection_ast
}  // namespace mongo
