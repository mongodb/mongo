// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/path.h"
#include "mongo/db/query/compiler/type_system/type.h"

namespace mongo {
class MatchExpression;
}  // namespace mongo

namespace mongo::pipeline::type_system {

/**
 * Returns the type of the document 'inputType' describes, refined by the knowledge that 'expr'
 * evaluated to 'assumeTrue'. An expression carrying no type information leaves 'inputType'
 * unchanged.
 * 'inputType' must be an object type, since it describes the document root. The returned type is an
 * object type too, or 'never' when the predicate cannot be satisfied by any document.
 */
Type narrowType(Type inputType, const MatchExpression* expr, bool assumeTrue);

namespace matcher {

/**
 * Returns 'inputType' refined by the knowledge that a leaf predicate on 'path', matching exactly
 * the values of 'constraint', evaluated to 'assumeTrue'.
 * Leaves 'inputType' unchanged for a path whose traversal is not modelled yet.
 */
Type narrowPath(Type inputType, const ElementPath& path, Type constraint, bool assumeTrue);

}  // namespace matcher

}  // namespace mongo::pipeline::type_system
