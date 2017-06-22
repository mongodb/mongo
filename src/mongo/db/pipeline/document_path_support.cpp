/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/pipeline/document_path_support.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace document_path_support {

namespace {

/**
 * If 'value' is an array, invokes 'callback' once on each element of 'value'. Otherwise, if 'value'
 * is not missing, invokes 'callback' on 'value' itself.
 */
void invokeCallbackOnTrailingValue(const Value& value,
                                   stdx::function<void(const Value&)> callback) {
    if (value.isArray()) {
        for (auto&& finalValue : value.getArray()) {
            if (!finalValue.missing()) {
                callback(finalValue);
            }
        }
    } else if (!value.missing()) {
        callback(value);
    }
}

void visitAllValuesAtPathHelper(Document doc,
                                const FieldPath& path,
                                size_t fieldPathIndex,
                                stdx::function<void(const Value&)> callback) {
    invariant(path.getPathLength() > 0 && fieldPathIndex < path.getPathLength());

    // The first field in the path must be treated as a field name, even if it is numeric as in
    // "0.a.1.b".
    auto nextValue = doc.getField(path.getFieldName(fieldPathIndex));
    ++fieldPathIndex;
    if (path.getPathLength() == fieldPathIndex) {
        invokeCallbackOnTrailingValue(nextValue, callback);
        return;
    }

    // Follow numeric field names as positions in array values. This loop consumes all consecutive
    // positional specifications, if applicable. For example, it will consume "0" and "1" from the
    // path "a.0.1.b" if the value at "a" is an array with arrays inside it.
    while (fieldPathIndex < path.getPathLength() && nextValue.isArray()) {
        if (auto index = parseUnsignedBase10Integer(path.getFieldName(fieldPathIndex))) {
            nextValue = nextValue[*index];
            ++fieldPathIndex;
        } else {
            break;
        }
    }

    if (fieldPathIndex == path.getPathLength()) {
        // The path ended in a positional traversal of arrays (e.g. "a.0").
        invokeCallbackOnTrailingValue(nextValue, callback);
        return;
    }

    if (nextValue.isArray()) {
        // The positional path specification ended at an array, or we did not have a positional
        // specification. In either case, there is still more path to explore, so we should go
        // through all elements and look for the rest of the path in any objects we encounter.
        //
        // Note we do not expand arrays within arrays this way. For example, {a: [[{b: 1}]]} has no
        // values on the path "a.b", but {a: [{b: 1}]} does.
        for (auto&& subValue : nextValue.getArray()) {
            if (subValue.getType() == BSONType::Object) {
                visitAllValuesAtPathHelper(subValue.getDocument(), path, fieldPathIndex, callback);
            }
        }
    } else if (nextValue.getType() == BSONType::Object) {
        visitAllValuesAtPathHelper(nextValue.getDocument(), path, fieldPathIndex, callback);
    }
}

}  // namespace

void visitAllValuesAtPath(const Document& doc,
                          const FieldPath& path,
                          stdx::function<void(const Value&)> callback) {
    visitAllValuesAtPathHelper(doc, path, 0, callback);
}

StatusWith<Value> extractElementAlongNonArrayPath(const Document& doc, const FieldPath& path) {
    invariant(path.getPathLength() > 0);
    Value curValue = doc.getField(path.getFieldName(0));
    if (curValue.getType() == BSONType::Array) {
        return {ErrorCodes::InternalError, "array along path"};
    }

    for (size_t i = 1; i < path.getPathLength(); i++) {
        curValue = curValue[path.getFieldName(i)];
        if (curValue.getType() == BSONType::Array) {
            return {ErrorCodes::InternalError, "array along path"};
        }
    }

    return curValue;
}

BSONObj documentToBsonWithPaths(const Document& input, const std::set<std::string>& paths) {
    BSONObjBuilder outputBuilder;
    for (auto&& path : paths) {
        // getNestedField does not handle dotted paths correctly, so instead of retrieving the
        // entire path, we just extract the first element of the path.
        const auto prefix = FieldPath::extractFirstFieldFromDottedPath(path);
        if (!outputBuilder.hasField(prefix)) {
            // Avoid adding the same prefix twice.
            input.getField(prefix).addToBsonObj(&outputBuilder, prefix);
        }
    }

    return outputBuilder.obj();
}

}  // namespace document_path_support
}  // namespace mongo
