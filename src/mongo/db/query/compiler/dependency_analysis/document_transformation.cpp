// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/document_transformation.h"

#include "mongo/db/pipeline/expression.h"

#include <algorithm>

namespace mongo::document_transformation {

boost::intrusive_ptr<Expression> ModifyPath::getExpression() const {
    return nullptr;
}

BSONDepthIndex RenamePath::getNewPathMaxArrayTraversals() const {
    return std::count(_newPath.begin(), _newPath.end(), '.');
}

BSONDepthIndex RenamePath::getOldPathMaxArrayTraversals() const {
    return std::count(_oldPath.begin(), _oldPath.end(), '.');
}

}  // namespace mongo::document_transformation
