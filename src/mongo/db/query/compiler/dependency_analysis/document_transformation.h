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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/modules.h"

#include <concepts>

#include <boost/intrusive_ptr.hpp>
#include <boost/noncopyable.hpp>

namespace mongo::document_transformation {

/**
 * A DocumentOperation is an abstract immutable view returned from describeTransformation.
 * These should not outlive the execution of the describeTransformation callback.
 *
 * The type hierarchy mirrors what we can cheaply compute and often need to distinguish.
 * - ReplaceRoot - the transformation does not start with copy of the old document
 * - PreservePath - the new path is the same as one in the old document
 * - RenamePath - the new path is defined as newPath: $oldPath
 * - ModifyPath - need to inspect to determine if it removes paths or computes or replaces with
 * constant, but cannot be
 */
class DocumentOperation : private boost::noncopyable {
public:
    DocumentOperation() = default;
    virtual ~DocumentOperation() = default;
};

/**
 * Describes an operation which starts with a blank document.
 * Can only ever be the first operation.
 * Note: For GetModifiedPaths, this represents a kAllPaths or kAllExcept projection.
 */
class ReplaceRoot final : public DocumentOperation {
public:
    /// When isEmpty is true, the new root is known to be the empty document. Any field not
    /// explicitly added by subsequent operations is definitely missing.
    explicit ReplaceRoot(bool isEmpty = false) : _isEmpty(isEmpty) {}

    /// Returns true if the new root is known to be the empty document, meaning any field not
    /// explicitly added by subsequent operations is definitely missing. When false, the new root
    /// may contain unknown fields (e.g. $replaceRoot: {newRoot: "$x"}).
    bool isEmpty() const {
        return _isEmpty;
    }

private:
    const bool _isEmpty;
};

/**
 * Describes a path preservation, such as for an inclusion projection.
 * PreservePath can only be emitted in conjunction with ReplaceRoot (which will be the first
 * operation).
 * Note: For GetModifiedPaths, this belongs under 'paths' with kAllExcept projection.
 */
class PreservePath final : public DocumentOperation {
public:
    explicit PreservePath(StringData path) : _path(path) {}

    /// The path being preserved.
    StringData getPath() const {
        return _path;
    }

private:
    const StringData _path;
};

/**
 * Describes a path modification.
 * This can include a trivial path removal (exclusion projection) or an expression computation,
 * which itself could remove the path ($$REMOVE case).
 * Note: For GetModifiedPaths, this belongs under 'paths' with kFiniteSet projection and is not
 * reported under kAllExcept.
 */
class ModifyPath : public DocumentOperation {
public:
    explicit ModifyPath(StringData path) : _path(path) {}

    /// The path being modified.
    StringData getPath() const {
        return _path;
    }

    /// True if the modification removes the path completely.
    virtual bool isRemoved() const {
        return false;
    }

    /// True if the new path value is the result of computation.
    virtual bool isComputed() const {
        return true;
    }

    /// Returns the Expression which determines the new value.
    virtual boost::intrusive_ptr<Expression> getExpression() const;

private:
    const StringData _path;
};

/**
 * Describes a path rename.
 * This can be a simple rename (a -> b) or it can be a rename where either/both the new and old path
 * contains dotted paths and thus may require array traversal.
 * Maps to 'paths' (new path is array), 'renames' (neither side is array) or 'paths' +
 * 'complexRenames' (old path is array).
 */
class RenamePath : public DocumentOperation {
public:
    RenamePath(StringData newPath, StringData oldPath) : _newPath(newPath), _oldPath(oldPath) {}

    /// The new name.
    StringData getNewPath() const {
        return _newPath;
    }

    /// The old name.
    StringData getOldPath() const {
        return _oldPath;
    }

    /**
     * Maximum number of array traversals along the new path.
     * Used to categorize this rename as simple / complex / other.
     */
    virtual BSONDepthIndex getNewPathMaxArrayTraversals() const;

    /**
     * Maximum number of array traversals along the old path.
     * Used to categorize this rename as simple / complex / other.
     */
    virtual BSONDepthIndex getOldPathMaxArrayTraversals() const;

private:
    const StringData _newPath;
    const StringData _oldPath;
};

/**
 * An interface which can be used with describeTransformation to enumerate the
 * DocumentOperations.
 */
class DocumentOperationVisitor {
public:
    virtual void operator()(const ReplaceRoot&) = 0;
    virtual void operator()(const ModifyPath&) = 0;
    virtual void operator()(const PreservePath&) = 0;
    virtual void operator()(const RenamePath&) = 0;
    virtual ~DocumentOperationVisitor() = default;

    /// Create a DocumentOperationVisitor from any callable.
    template <typename Visitor>
    static auto create(Visitor&& visitor) {
        class DocumentOperationVisitorImpl final : public DocumentOperationVisitor {
        public:
            explicit DocumentOperationVisitorImpl(Visitor&& visitor) noexcept(
                std::is_nothrow_move_constructible_v<std::decay_t<Visitor>>)
                : visitor(std::forward<Visitor>(visitor)) {}
            void operator()(const ReplaceRoot& op) override {
                visitor(op);
            }
            void operator()(const ModifyPath& op) override {
                visitor(op);
            }
            void operator()(const PreservePath& op) override {
                visitor(op);
            }
            void operator()(const RenamePath& op) override {
                visitor(op);
            }
            std::decay_t<Visitor> visitor;
        };
        return DocumentOperationVisitorImpl{std::forward<Visitor>(visitor)};
    }
};

/**
 * Defines the methods required to describe the effects of a single document transformation.
 *
 * The transformation is represented as a series of DocumentOperations which determine the kind of
 * change to the source document. These operations provide a superset of the information available
 * in DocumentSource::GetModPathsReturn.
 *
 * The main method for obtaining the description is 'describeTransformation', which receives a
 * visitor instance. The visitor is called sequentially with the DocumentOperations needed to arrive
 * at a correct matching logical transformation.
 *
 * The DocumentOperation interfaces logically extend the getModifiedPaths return type by
 * allowing the caller to know more information about the kind of modification, such as, if it is a
 * computation, and if so, the defining Expression, if known. The rename information is more
 * complete, since any kind of rename operation can be reported (including 'a.b.c' -> 'x.y.z' which
 * is traditionally treated as an opaque modification).
 *
 * Here is how they correspond to GetModPathsReturn::Type.
 *
 * GetModPathsReturn::Type::kNotSupported:
 *  -> visitor called with ReplaceRoot (same as kAllPaths)
 * GetModPathsReturn::Type::kAllPaths:
 *  -> visitor called with ReplaceRoot
 * GetModPathsReturn::Type::kAllExcept:
 *  -> visitor called with ReplaceRoot
 *  -> visitor called with PreservePath for every preserved path
 *  -> visitor called with RenamePath for every rename
 * GetModPathsReturn::Type::kFiniteSet:
 *  -> visitor called with ModifyPath for every modified path
 *  -> visitor called with RenamePath for every rename
 *
 * Since the returned information is a superset of the GetModifiedPathsReturn state, it is possible
 * to use the describeTransformation method to construct it using a helper 'toGetModPathsReturn'.
 *
 * The reverse direction is also possible - we can construct a GetModifiedPathsReturn, which may be
 * a lossy conversion (GetModifiedPathsReturn cannot represent combinations such as kAllExcept with
 * explicit modified paths).
 *
 * Note that each path is reported only once. This is in contrast with GetModPathsReturn, where
 * 'complexRenames' are also reported in 'paths'. The correct operation here is to call the visitor
 * once with RenamePath with correct array traversal information for both sides of the rename, which
 * preserves the semantics.
 */
template <typename T>
concept DescribesDocumentTransformation = requires(T t, DocumentOperationVisitor& visitor) {
    { t.describeTransformation(visitor) } -> std::same_as<void>;
};

/**
 * Describes the transformation performed on the document.
 */
template <DescribesDocumentTransformation T>
void describeTransformation(DocumentOperationVisitor& visitor, const T& t) {
    return t.describeTransformation(visitor);
}

/**
 * Describes the transformation performed on the document, using a callable object.
 */
template <DescribesDocumentTransformation T, typename Visitor>
void describeTransformation(Visitor&& visitor, const T& t)
requires(!std::derived_from<Visitor, DocumentOperationVisitor>)
{
    auto wrapped = DocumentOperationVisitor::create(std::ref(visitor));
    return t.describeTransformation(wrapped);
}

}  // namespace mongo::document_transformation
