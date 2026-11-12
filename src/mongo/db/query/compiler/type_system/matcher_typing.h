// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/path.h"
#include "mongo/db/query/compiler/type_system/type.h"

namespace mongo {
class MatchExpression;
}  // namespace mongo

namespace mongo::pipeline::type_system {

/// Restricts narrowing to predicates which may narrow the type of the specified path. An empty path
/// denotes that we want to narrow every predicate.
struct PathFilter {
    FieldRef path;
    /// Special case for when we only care about whether 'path' can have arrays in it. This allows
    /// us to filter out more predicates.
    bool arraynessOnly = false;

    /// Returns true if a predicate on 'predicatePath', matching exactly the values of 'constraint',
    /// can refine the type of 'path' when it evaluates to 'assumeTrue'.
    bool canRefine(const ElementPath& predicatePath, const Type& constraint, bool assumeTrue) const;
};

/**
 * Returns the type of the document 'inputType' describes, refined by the knowledge that 'expr'
 * evaluated to 'assumeTrue'. An expression carrying no type information leaves 'inputType'
 * unchanged. Predicates which cannot refine the type of the path specified in 'filter' are skipped.
 * 'inputType' must be an object type, since it describes the document root. The returned type is an
 * object type too, or 'never' when the predicate cannot be satisfied by any document.
 */
Type narrowType(Type inputType,
                const MatchExpression* expr,
                bool assumeTrue,
                const PathFilter& filter = {});

namespace matcher {

/**
 * Returns 'inputType' refined by the knowledge that a leaf predicate on 'path', matching exactly
 * the values of 'constraint', evaluated to 'assumeTrue'.
 * Leaves 'inputType' unchanged for a path whose traversal is not modelled yet.
 */
Type narrowPath(Type inputType, const ElementPath& path, Type constraint, bool assumeTrue);

}  // namespace matcher

}  // namespace mongo::pipeline::type_system
