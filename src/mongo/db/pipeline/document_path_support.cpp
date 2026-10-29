// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_path_support.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <string_view>

namespace mongo {
namespace document_path_support {

namespace {

/**
 * If 'value' is an array, invokes 'callback' once on each element of 'value'. Otherwise, if 'value'
 * is not missing, invokes 'callback' on 'value' itself.
 */
void invokeCallbackOnTrailingValue(const Value& value,
                                   const std::function<void(const Value&)>& callback) {
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
    auto pathLength = path.getPathLength();

    tassert(11294814,
            str::stream() << "Expect path index to be in range (0, " << pathLength << "), got "
                          << fieldPathIndex,
            pathLength > 0 && fieldPathIndex < pathLength);

    // The first field in the path must be treated as a field name, even if it is numeric as in
    // "0.a.1.b".
    auto nextValue = doc.getField(path.getFieldName(fieldPathIndex));
    ++fieldPathIndex;
    if (pathLength == fieldPathIndex) {
        invokeCallbackOnTrailingValue(nextValue, callback);
        return;
    }

    // Follow numeric field names as positions in array values. This loop consumes all consecutive
    // positional specifications, if applicable. For example, it will consume "0" and "1" from the
    // path "a.0.1.b" if the value at "a" is an array with arrays inside it.
    while (fieldPathIndex < pathLength && nextValue.isArray()) {
        const std::string_view field = path.getFieldName(fieldPathIndex);
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

    if (fieldPathIndex == pathLength) {
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
