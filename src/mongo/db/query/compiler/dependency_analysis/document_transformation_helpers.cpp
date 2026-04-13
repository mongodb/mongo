/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/dependency_analysis/document_transformation_helpers.h"

#include "mongo/util/assert_util.h"

namespace mongo::document_transformation {

/**
 * Modify path which defines the new value using an Expression.
 */
class ExpressionModifyPath final : public ModifyPath {
public:
    ExpressionModifyPath(StringData path, boost::intrusive_ptr<Expression> expr)
        : ModifyPath(path), _expr(expr) {}

    bool isRemoved() const override {
        return false;
    }
    bool isComputed() const override {
        return true;
    }
    boost::intrusive_ptr<Expression> getExpression() const override {
        return _expr;
    }

private:
    const boost::intrusive_ptr<Expression> _expr;
};

/**
 * A simple path removal, not a modification to $$REMOVE.
 */
class RemovePath final : public ModifyPath {
public:
    explicit RemovePath(StringData path) : ModifyPath(path) {}

    bool isRemoved() const override {
        return true;
    }
    bool isComputed() const override {
        return false;
    }
    boost::intrusive_ptr<Expression> getExpression() const override {
        return nullptr;
    }
};

namespace detail {

void describeProjectedPath(DocumentOperationVisitor& visitor, StringData path, bool isInclusion) {
    if (isInclusion) {
        visitor(document_transformation::PreservePath{path});
    } else {
        visitor(document_transformation::RemovePath{path});
    }
}

/**
 * Visitor for Expressions which reports paths to a DocumentOperationVisitor for supported
 * expressions. This fills in information which Expression::getComputedPaths does not report, but
 * which the DocumentOperationVisitor can interpret, such as "other" renames like 'a.b': '$c.d.e'.
 * If the Expression type is not supported, isOK() returns false, and the DocumentOperationVisitor
 * is not called.
 */
class SpecializedExpressionOperationVisitor : public SelectiveConstExpressionVisitorBase {
public:
    using SelectiveConstExpressionVisitorBase::visit;

    explicit SpecializedExpressionOperationVisitor(DocumentOperationVisitor& visitor,
                                                   StringData path,
                                                   BSONDepthIndex depth)
        : _visitor(visitor), _path(path), _depth(depth) {}

    /**
     * Returns true if DocumentOperationVisitor was called during the visit.
     */
    bool isOK() const {
        return _handled;
    }

    void visit(const ExpressionFieldPath* expr) final {
        if (expr->isVariableReference()) {
            return;
        }
        const FieldPath& oldFieldPath = expr->getFieldPath();
        if (oldFieldPath.getPathLength() == 1) {
            return;
        }
        _handled = true;
        StringData oldPath = oldFieldPath.tailPath();
        BSONDepthIndex oldPathMaxArrayTraversals = std::count(oldPath.begin(), oldPath.end(), '.');
        _visitor(RenamePathWithFixedArrayness{_path, oldPath, _depth, oldPathMaxArrayTraversals});
    }

    void visit(const ExpressionObject* expr) final {
        _handled = true;
        const std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>&>>& children =
            expr->getChildExpressions();
        for (auto&& [childField, childExpr] : children) {
            auto childPath = FieldPath::getFullyQualifiedPath(_path, childField);
            // The depth is used to report the maximum levels of array traversals on the left side.
            // Since object expressions evaluate to objects, we know they will not require
            // additional array traversal.
            describeComputedPath(_visitor, childPath, childExpr, _depth);
        }
    }

private:
    DocumentOperationVisitor& _visitor;
    const StringData _path;
    const BSONDepthIndex _depth;
    bool _handled{false};
};

/**
 * Reports the computed paths of 'expr' into the 'visitor'.
 * For some expression types, this may report more detailed information than
 * Expression::getComputedPaths (since that does not report some rename cases).
 */
void describeComputedPath(DocumentOperationVisitor& visitor,
                          const std::string& path,
                          const boost::intrusive_ptr<Expression>& expr,
                          BSONDepthIndex depth) {
    SpecializedExpressionOperationVisitor exprVisitor{visitor, path, depth};
    expr->acceptVisitor(&exprVisitor);
    if (exprVisitor.isOK()) {
        return;
    }

    auto exprComputedPaths = expr->getComputedPaths(path);
    for (auto&& exprComputedPath : exprComputedPaths.paths) {
        if (!exprComputedPaths.complexRenames.contains(exprComputedPath)) {
            visitor(document_transformation::ExpressionModifyPath{exprComputedPath, expr});
        }
    }
    for (auto&& [newPath, oldPath] : exprComputedPaths.renames) {
        visitor(RenamePathWithFixedArrayness{newPath, oldPath, depth, 0});
    }
    for (auto&& [newPath, oldPath] : exprComputedPaths.complexRenames) {
        visitor(RenamePathWithFixedArrayness{newPath, oldPath, depth, 1});
    }
}

RenamePathWithFixedArrayness::RenamePathWithFixedArrayness(StringData newPath,
                                                           StringData oldPath,
                                                           BSONDepthIndex newPathMaxArrayTraversals,
                                                           BSONDepthIndex oldPathMaxArrayTraversals)
    : RenamePath(newPath, oldPath),
      _newPathMaxArrayTraversals(newPathMaxArrayTraversals),
      _oldPathMaxArrayTraversals(oldPathMaxArrayTraversals) {}

}  // namespace detail

void describeGetModPathsReturn(DocumentOperationVisitor& visitor,
                               const GetModPathsReturnType& type,
                               const OrderedPathSet& paths,
                               const StringMap<std::string>& renames,
                               const StringMap<std::string>& complexRenames) {
    switch (type) {
        case GetModPathsReturnType::kNotSupported:
        case GetModPathsReturnType::kAllPaths:
            visitor(ReplaceRoot{});
            return;
        case GetModPathsReturnType::kAllExcept:
            // kAllExcept means preserved fields are known, but doesn't give any guarantees about
            // the fields which are not listed.
            visitor(ReplaceRoot{});
            break;
        case GetModPathsReturnType::kFiniteSet:
            break;
    }

    for (const auto& path : paths) {
        // A path in 'paths' can be:
        // - only a preserved path, if kAllExcept
        // - a modified path OR a complexRename, otherwise
        if (type == GetModPathsReturnType::kAllExcept) {
            visitor(PreservePath{path});
        } else if (!complexRenames.contains(path)) {
            // The DocumentOperationVisitor does not see complexRenames as modifications.
            // Instead, it will see them as a RenamePath operation with the appropriate flags.
            visitor(ModifyPath{path});
        }
    }
    for (const auto& [newPath, oldPath] : renames) {
        // Simple renames means that neither path can be an array.
        visitor(detail::RenamePathWithFixedArrayness{newPath, oldPath, 0, 0});
    }
    for (const auto& [newPath, oldPath] : complexRenames) {
        // Complex renames means that oldPath can be an array and the new path cannot be.
        visitor(detail::RenamePathWithFixedArrayness{newPath, oldPath, 0, 1});
    }
}

void detail::GetModPathsReturnConverter::operator()(const ReplaceRoot&) {
    tassert(11938001,
            "When present, ReplaceRoot must be the first operation",
            type == GetModPathsReturnType::kNotSupported);
    type = GetModPathsReturnType::kAllPaths;
}

void detail::GetModPathsReturnConverter::operator()(const ModifyPath& op) {
    switch (type) {
        case GetModPathsReturnType::kNotSupported:
            type = GetModPathsReturnType::kFiniteSet;
            break;
        case GetModPathsReturnType::kAllPaths:
            // If we see ModifyPath after replace root, then this is an inclusion
            // projection too, but we cannot report the modifications.
            type = GetModPathsReturnType::kAllExcept;
            return;
        case GetModPathsReturnType::kAllExcept:
            // GetModPathsReturn does not store modified paths when kAllExcept.
            return;
        case GetModPathsReturnType::kFiniteSet:
            break;
    }
    paths.emplace(op.getPath());
}

void detail::GetModPathsReturnConverter::operator()(const PreservePath& op) {
    switch (type) {
        case GetModPathsReturnType::kNotSupported:
            tassert(11938000,
                    "Expected ReplaceRoot before PreservePath",
                    type != GetModPathsReturnType::kNotSupported);
            break;
        case GetModPathsReturnType::kAllPaths:
            type = GetModPathsReturnType::kAllExcept;
            break;
        case GetModPathsReturnType::kAllExcept:
            break;
        case GetModPathsReturnType::kFiniteSet:
            // GetModPathsReturn does not store preserved paths when kFiniteSet.
            return;
    }
    type = GetModPathsReturnType::kAllExcept;
    paths.emplace(op.getPath());
}

void detail::GetModPathsReturnConverter::operator()(const RenamePath& op) {
    switch (type) {
        case GetModPathsReturnType::kNotSupported:
            type = GetModPathsReturnType::kFiniteSet;
            break;
        case GetModPathsReturnType::kAllPaths:
            type = GetModPathsReturnType::kAllExcept;
            break;
        case GetModPathsReturnType::kAllExcept:
            break;
        case GetModPathsReturnType::kFiniteSet:
            break;
    }
    bool pathsIsModifications = type == GetModPathsReturnType::kFiniteSet;
    BSONDepthIndex newPathArrayTraversals = op.getNewPathMaxArrayTraversals();
    BSONDepthIndex oldPathArrayTraversals = op.getOldPathMaxArrayTraversals();
    if (newPathArrayTraversals != 0) {
        if (pathsIsModifications) {
            paths.emplace(op.getNewPath());
        }
    } else if (oldPathArrayTraversals != 0) {
        if (pathsIsModifications) {
            paths.emplace(op.getNewPath());
        }
        if (oldPathArrayTraversals == 1) {
            complexRenames.emplace(op.getNewPath(), op.getOldPath());
        }
    } else {
        renames.emplace(op.getNewPath(), op.getOldPath());
    }
}

GetModPathsReturn detail::GetModPathsReturnConverter::done() && {
    if (type == GetModPathsReturnType::kNotSupported && paths.empty() && renames.empty() &&
        complexRenames.empty()) {
        type = GetModPathsReturnType::kFiniteSet;
    }
    return {type, std::move(paths), std::move(renames), std::move(complexRenames)};
}

}  // namespace mongo::document_transformation
