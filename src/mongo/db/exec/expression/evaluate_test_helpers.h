// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
namespace expression_evaluation_test {

/**
 * Creates an expression given by 'expressionName' and evaluates it using
 * 'operands' as inputs, returning the result.
 */
Value evaluateExpression(const std::string& expressionName,
                         const std::vector<ImplicitValue>& operands);

/**
 * Takes the name of an expression as its first argument and a list of pairs of arguments and
 * expected results as its second argument, and asserts that for the given expression the arguments
 * evaluate to the expected results.
 */
void assertExpectedResults(
    const std::string& expression,
    std::initializer_list<std::pair<std::initializer_list<ImplicitValue>, ImplicitValue>>
        operations);

/** Convert Value to a wrapped BSONObj with an empty string field name. */
BSONObj toBson(const Value& value);

/** Convert Document to BSON. */
BSONObj toBson(const Document& document);

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj);

/** Convert a serialized JSON into a Document object.  */
Document fromJson(const std::string& json);

}  // namespace expression_evaluation_test
}  // namespace mongo
