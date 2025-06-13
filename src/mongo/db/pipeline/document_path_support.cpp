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

#include "mongo/db/pipeline/document_path_support.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace document_path_support {

namespace {

/**
 * If 'value' is an array, invokes 'callback' once on each element of 'value'. Otherwise, if 'value'
 * is not missing, invokes 'callback' on 'value' itself.
 */
void invokeCallbackOnTrailingValue(const Value& value, std::function<void(const Value&)> callback) {
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

void visitAllValuesAtPathHelper(const Document& doc,
                                const FieldPath& path,
                                size_t fieldPathIndex,
                                std::function<void(const Value&)> callback) {
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
        const StringData field = path.getFieldName(fieldPathIndex);
        // Check for a numeric component that is not prefixed by 0 (for example "1" rather than
        // "01"). These should act as field names, not as an index into an array.
        if (auto index = str::parseUnsignedBase10Integer(field);
            index && FieldRef::isNumericPathComponentStrict(field)) {
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
            if (subValue.getType() == BSONType::object) {
                visitAllValuesAtPathHelper(subValue.getDocument(), path, fieldPathIndex, callback);
            }
        }
    } else if (nextValue.getType() == BSONType::object) {
        visitAllValuesAtPathHelper(nextValue.getDocument(), path, fieldPathIndex, callback);
    }
}

}  // namespace

void visitAllValuesAtPath(const Document& doc,
                          const FieldPath& path,
                          std::function<void(const Value&)> callback) {
    visitAllValuesAtPathHelper(doc, path, 0, callback);
}

}  // namespace document_path_support
}  // namespace mongo
