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

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/util/str.h"

namespace mongo {
namespace projection_ast {

/**
 * Summary of the dependency analysis done on the projection AST.
 */
struct ProjectionDependencies {
    // Whether MatchDetails of the query's filter are required.
    bool requiresMatchDetails = false;

    // Whether the entire document is required to do the projection.
    bool requiresDocument = false;
    // Which fields are necessary to perform the projection, or boost::none if all are required.
    boost::optional<std::vector<std::string>> requiredFields;

    bool hasDottedPath = false;

    QueryMetadataBitSet metadataRequested;
};

/**
 * Used to represent a projection for dependency analysis and query planning.
 */
enum class ProjectType { kInclusion, kExclusion };
class Projection {
public:
    Projection(ProjectionPathASTNode root, ProjectType type);

    const ProjectionPathASTNode* root() const {
        return &_root;
    }

    ProjectionPathASTNode* root() {
        return &_root;
    }

    ProjectType type() const {
        return _type;
    }

    /**
     * Returns true if the projection requires match details from the query,
     * and false otherwise.
     */
    bool requiresMatchDetails() const {
        return _deps.requiresMatchDetails;
    }

    /**
     * Returns whether the full document is required to compute this projection.
     */
    bool requiresDocument() const {
        return _deps.requiresDocument;
    }

    /**
     * Return which fields are required to compute the projection, assuming the entire document is
     * not needed.
     */
    const std::vector<std::string>& getRequiredFields() const {
        invariant(_type == ProjectType::kInclusion);
        return *_deps.requiredFields;
    }

    const QueryMetadataBitSet& metadataDeps() const {
        return _deps.metadataRequested;
    }

    /**
     * Returns true if the element at 'path' is preserved entirely after this projection is applied,
     * and false otherwise. For example, the projection {a: 1} will preserve the element located at
     * 'a.b', and the projection {'a.b': 0} will not preserve the element located at 'a'.
     */
    bool isFieldRetainedExactly(StringData path);

    /**
     * A projection is considered "simple" if it doesn't require the full document, operates only
     * on top-level fields, has no positional projection, and doesn't require the sort key.
     */
    bool isSimple() const {
        return !_deps.hasDottedPath && !_deps.requiresMatchDetails &&
            !_deps.metadataRequested.any() && !_deps.requiresDocument;
    }

private:
    ProjectionPathASTNode _root;
    ProjectType _type;
    ProjectionDependencies _deps;
};

}  // namespace projection_ast
}  // namespace mongo
