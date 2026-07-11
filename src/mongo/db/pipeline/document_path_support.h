// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>

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
 * The template parameter 'PathsHaveUniqueFirstFields' controls whether or not first fields in the
 * result BSONObj will be checked for uniqueness. Setting it to 'true' can be used as a performance
 * optimization in case it is known that all first fields in the result object will be unique.
 */
template <bool PathsHaveUniqueFirstFields = false>
void documentToBsonWithPaths(const Document& input,
                             const OrderedPathSet& paths,
                             BSONObjBuilder* builder) {
    boost::optional<std::string_view> prevFirstField = boost::none;
    for (auto&& path : paths) {
        // getNestedField does not handle dotted paths correctly, so instead of retrieving the
        // entire path, we just extract the first element of the path.
        auto firstField = FieldPath::extractFirstFieldFromDottedPath(path);

        // Avoid adding the same first field twice. Because OrderedPathSet orders parent paths
        // directly before their children, it suffices to check the preceding toplevel field.
        if (PathsHaveUniqueFirstFields || firstField != prevFirstField) {
            input.getField(firstField).addToBsonObj(builder, firstField);
        }
        if constexpr (!PathsHaveUniqueFirstFields) {
            prevFirstField = firstField;
        }
    }
}

/**
 * Converts a 'Document' to a BSON object.
 *
 * The template parameter 'PathsHaveUniqueFirstFields' controls whether or not first fields in the
 * result BSONObj will be checked for uniqueness. Setting it to 'true' can be used as a performance
 * optimization in case it is known that all first fields in the result object will be unique.
 */
template <typename BSONTraits = BSONObj::DefaultSizeTrait, bool PathsHaveUniqueFirstFields = false>
BSONObj documentToBsonWithPaths(const Document& input, const OrderedPathSet& paths) {
    BSONObjBuilder outputBuilder;
    documentToBsonWithPaths<PathsHaveUniqueFirstFields>(input, paths, &outputBuilder);
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
