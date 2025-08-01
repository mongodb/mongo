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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <functional>

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
 * Extracts 'paths' from the input document and returns a BSON object containing only those paths.
 *
 * The template parameter 'EnsureUniquePrefixes' controls whether or not prefixes in the result
 * BSONObj will be checked for uniqueness. Setting it to 'false' can be used as a performance
 * optimization in case it is known that all prefixes in the result object will be unique.
 * Note: the version that needs to ensure that prefixes are unique has quadratic asymptotic
 * complexity in the worst case.
 */
template <bool EnsureUniquePrefixes = true>
void documentToBsonWithPaths(const Document& input,
                             const OrderedPathSet& paths,
                             BSONObjBuilder* builder) {
    for (auto&& path : paths) {
        // getNestedField does not handle dotted paths correctly, so instead of retrieving the
        // entire path, we just extract the first element of the path.
        auto prefix = FieldPath::extractFirstFieldFromDottedPath(path);

        // Avoid adding the same prefix twice. Note: 'hasField()' iterates over all existing
        // fields in the builder until it finds a field with the given name or it reaches the
        // end of the object.
        if (!EnsureUniquePrefixes || !builder->hasField(prefix)) {
            input.getField(prefix).addToBsonObj(builder, prefix);
        }
    }
}

/**
 * Converts a 'Document' to a BSON object.
 *
 * The template parameter 'EnsureUniquePrefixes' controls whether or not prefixes in the result
 * BSONObj will be checked for uniqueness. Setting it to 'false' can be used as a performance
 * optimization in case it is known that all prefixes in the result object will be unique.
 */
template <typename BSONTraits = BSONObj::DefaultSizeTrait, bool EnsureUniquePrefixes = true>
BSONObj documentToBsonWithPaths(const Document& input, const OrderedPathSet& paths) {
    BSONObjBuilder outputBuilder;
    documentToBsonWithPaths<EnsureUniquePrefixes>(input, paths, &outputBuilder);
    BSONObj docBSONObj = outputBuilder.obj<BSONTraits>();
    Document::validateDocumentBSONSize(docBSONObj, BSONTraits::MaxSize);
    return docBSONObj;
}

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
