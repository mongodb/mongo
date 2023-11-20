/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/hash/hash.h>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/assert_util.h"

namespace mongo::sbe::vm {

// MakeObj input cursor used when there are individual input fields but no input object.
class InputFieldsOnlyCursor {
public:
    using InputFields = MakeObjCursorInputFields;

    InputFieldsOnlyCursor(const StringListSet& fields, const InputFields& values, size_t numItems)
        : _values(values), _numItems(numItems) {
        moveNext(fields);
    }

    MONGO_COMPILER_ALWAYS_INLINE bool atEnd() const {
        return _reachedEnd;
    }

    MONGO_COMPILER_ALWAYS_INLINE void moveNext(const StringListSet& fields) {
        while (_current < _numItems) {
            size_t pos = _current;
            ++_current;

            auto [_, tag, val] = _values[pos];

            if (tag != value::TypeTags::Nothing) {
                _fieldIdx = pos;
                _name = fields[pos];
                _tag = tag;
                _val = val;
                return;
            }
        }

        _reachedEnd = true;
    }

    MONGO_COMPILER_ALWAYS_INLINE StringData fieldName() const {
        return _name;
    }

    MONGO_COMPILER_ALWAYS_INLINE size_t fieldIdx() const {
        return _fieldIdx;
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> value() const {
        return {_tag, _val};
    }

    MONGO_COMPILER_ALWAYS_INLINE void appendTo(UniqueBSONObjBuilder& bob) const {
        auto [tag, val] = value();
        bson::appendValueToBsonObj(bob, _name, tag, val);
    }

private:
    const InputFields& _values;

    size_t _current = 0;
    size_t _numItems = 0;
    bool _reachedEnd = false;

    size_t _fieldIdx = 0;
    StringData _name = ""_sd;
    value::TypeTags _tag = value::TypeTags::Nothing;
    value::Value _val{0u};
};

template <typename ObjectCursorT>
class ObjectAndInputFieldsCursorBase {
public:
    using InputFields = MakeObjCursorInputFields;

    ObjectAndInputFieldsCursorBase(const StringListSet& fields,
                                   const InputFields& values,
                                   ObjectCursorT objectCursor)
        : _values(values), _objectCursor(std::move(objectCursor)) {
        moveNext(fields);
    }

    MONGO_COMPILER_ALWAYS_INLINE bool atEnd() const {
        return _reachedEnd;
    }

    MONGO_COMPILER_ALWAYS_INLINE void moveNext(const StringListSet& fields) {
        while (!_objectCursor.atEnd()) {
            StringData name = _objectCursor.fieldName();
            auto [fieldFromObjTag, fieldFromObjVal] = _objectCursor.value();

            _objectCursor.moveNext(fields);

            size_t pos = fields.findPos(name);

            if (pos >= _values.size()) {
                _fieldIdx = pos;
                _name = name;
                _tag = fieldFromObjTag;
                _val = fieldFromObjVal;
                return;
            } else {
                auto [_, tag, val] = _values[pos];

                if (tag != value::TypeTags::Nothing) {
                    _fieldIdx = pos;
                    _name = name;
                    _tag = tag;
                    _val = val;
                    return;
                }
            }
        }

        _reachedEnd = true;
    }

    MONGO_COMPILER_ALWAYS_INLINE StringData fieldName() const {
        return _name;
    }

    MONGO_COMPILER_ALWAYS_INLINE size_t fieldIdx() const {
        return _fieldIdx;
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> value() const {
        return {_tag, _val};
    }

    MONGO_COMPILER_ALWAYS_INLINE void appendTo(UniqueBSONObjBuilder& bob) const {
        auto [tag, val] = value();
        bson::appendValueToBsonObj(bob, _name, tag, val);
    }

private:
    const InputFields& _values;

    ObjectCursorT _objectCursor;
    bool _reachedEnd = false;

    size_t _fieldIdx = 0;
    StringData _name = ""_sd;
    value::TypeTags _tag = value::TypeTags::Nothing;
    value::Value _val{0u};
};

// MakeObj input cursor used when the input object is a BSON object and there are also individual
// input fields.
class BsonObjWithInputFieldsCursor : public ObjectAndInputFieldsCursorBase<BsonObjCursor> {
public:
    using BaseT = ObjectAndInputFieldsCursorBase<BsonObjCursor>;
    using BaseT::BaseT;
};

// MakeObj input cursor used when the input object is an SBE object and there are also individual
// input fields.
class ObjWithInputFieldsCursor : public ObjectAndInputFieldsCursorBase<ObjectCursor> {
public:
    using BaseT = ObjectAndInputFieldsCursorBase<ObjectCursor>;
    using BaseT::BaseT;
};

}  // namespace mongo::sbe::vm
