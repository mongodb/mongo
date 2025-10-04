/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document.h"

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
