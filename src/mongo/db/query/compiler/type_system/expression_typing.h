// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/compiler/type_system/type.h"

namespace mongo::pipeline::type_system {

/**
 * Returns the output type of the expression when evaluated on a document of type 'inputType'.
 * A FieldPathExpression returns the type of the path, when evaluated on the document 'inputType'.
 * A CompareExpression of type $eq always evaluates to a boolean, regardless of the input.
 */
Type getOutputType(const Type& inputType, const Expression* expr);

/**
 * Returns the type of the document 'inputType' describes, refined by the knowledge that 'expr'
 * evaluated to 'assumeTrue'. An expression carrying no type information returns 'inputType'
 * unchanged.
 * 'inputType' must be an object type, since it describes the document root. The returned type
 * is an object type too, or 'never' when the predicate cannot be satisfied by any document.
 * An expression like {$eq: ['$a', 1]} which is known to evaluate to 'assumeTrue' can narrow the
 * type of the field 'a' in the document 'inputType'.
 */
Type narrowType(Type inputType, const Expression* expr, bool assumeTrue);

}  // namespace mongo::pipeline::type_system
