// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace projection_ast {
/**
 * This is intended to be used for debug output, not for serialization.
 */
BSONObj astToDebugBSON(const ASTNode* root);

BSONObj serialize(const ProjectionPathASTNode& root,
                  const query_shape::SerializationOptions& options);
}  // namespace projection_ast
}  // namespace mongo
