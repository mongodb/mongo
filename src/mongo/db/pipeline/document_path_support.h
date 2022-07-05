/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <vector>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {
namespace document_path_support {

/**
 * Calls 'callback' once for each value found at 'path' in the document 'doc'. If an array value is
 * found at 'path', it is expanded and 'callback' is invoked once for each value within the array.
 *
 * For example, 'callback' will be invoked on the values 1, 1, {a: 1}, 2 and 3 are on the path "x.y"
 * in the document {x: [{y: 1}, {y: 1}, {y: {a: 1}}, {y: [2, 3]}, 3, 4]}.
 */
void visitAllValuesAtPath(const Document& doc,
                          const FieldPath& path,
                          std::function<void(const Value&)> callback);

/**
 * Returns the element at 'path' in 'doc', or a missing Value if the path does not fully exist.
 *
 * Returns ErrorCodes::InternalError if an array is encountered along the path or at the end of the
 * path.
 */
StatusWith<Value> extractElementAlongNonArrayPath(const Document& doc, const FieldPath& path);

/**
 * Extracts 'paths' from the input document and returns a BSON object containing only those paths.
 */
BSONObj documentToBsonWithPaths(const Document&, const OrderedPathSet& paths);

/**
 * Extracts 'paths' from the input document to a flat document.
 *
 * This does *not* traverse arrays when extracting values from dotted paths. For example, the path
 * "a.b" will not populate the result document if the input document is {a: [{b: 1}]}.
 */
template <class Container>
Document extractPathsFromDoc(const Document& doc, const Container& paths) {
    MutableDocument result;
    for (const auto& field : paths) {
        result.addField(field.fullPath(), doc.getNestedField(field));
    }
    return result.freeze();
}

}  // namespace document_path_support
}  // namespace mongo
