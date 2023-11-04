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
#include "mongo/util/assert_util.h"

namespace mongo::sbe::vm {

class MakeObjCursorInputFields;

// MakeObj input cursor for BSON objects.
class BsonObjCursor {
public:
    using InputFields = MakeObjCursorInputFields;

    BsonObjCursor(const StringListSet& fields,
                  const std::vector<MakeObjSpec::FieldAction>& actions,
                  MakeObjSpec::ActionType defActionType,
                  const char* be)
        : _fields(fields), _actions(actions), _defActionType(defActionType), _be(be) {
        _last = _be + ConstDataView(_be).read<LittleEndian<uint32_t>>() - 1;
        _be += 4;
        if (_be != _last) {
            _name = bson::fieldNameAndLength(_be);
            _nextBe = bson::advance(_be, _name.size());
        }
    }

    inline bool atEnd() const {
        return _be == _last;
    }
    inline void moveNext() {
        _be = _nextBe;
        if (_be != _last) {
            _name = bson::fieldNameAndLength(_be);
            _nextBe = bson::advance(_be, _name.size());
        }
    }
    inline StringData fieldName() const {
        return _name;
    }
    inline std::pair<size_t, MakeObjSpec::ActionType> fieldIdxAndType() const {
        // Look up '_name' in the '_fields' set.
        size_t fieldIdx = _fields.findPos(_name);
        // Return the index ('npos' if not found) and the ActionType for this field.
        auto type = fieldIdx != StringListSet::npos ? _actions[fieldIdx].type() : _defActionType;
        return {fieldIdx, type};
    }
    inline std::pair<value::TypeTags, value::Value> value() const {
        return bson::convertFrom<true>(bsonElement());
    }
    inline void appendTo(UniqueBSONObjBuilder& bob) const {
        bob.append(bsonElement());
    }

private:
    inline BSONElement bsonElement() const {
        auto fieldNameLenWithNull = _name.size() + 1;
        auto totalSize = _nextBe - _be;
        return BSONElement(_be, fieldNameLenWithNull, totalSize, BSONElement::TrustedInitTag{});
    }

    const StringListSet& _fields;
    const std::vector<MakeObjSpec::FieldAction>& _actions;
    MakeObjSpec::ActionType _defActionType;

    const char* _be{nullptr};
    const char* _nextBe{nullptr};
    const char* _last{nullptr};
    StringData _name;
};

// MakeObj input cursor for SBE objects.
class ObjectCursor {
public:
    using InputFields = MakeObjCursorInputFields;

    ObjectCursor(const StringListSet& fields,
                 const std::vector<MakeObjSpec::FieldAction>& actions,
                 MakeObjSpec::ActionType defActionType,
                 value::Object* objRoot)
        : _fields(fields),
          _actions(actions),
          _defActionType(defActionType),
          _objRoot(objRoot),
          _idx(0),
          _endIdx(_objRoot->size()) {}

    inline bool atEnd() const {
        return _idx == _endIdx;
    }
    inline void moveNext() {
        ++_idx;
    }
    inline StringData fieldName() const {
        return StringData(_objRoot->field(_idx));
    }
    inline std::pair<size_t, MakeObjSpec::ActionType> fieldIdxAndType() const {
        // Look up '_name' in the '_fields' set.
        size_t fieldIdx = _fields.findPos(fieldName());
        // Return the index ('npos' if not found) and the ActionType for this field.
        auto type = fieldIdx != StringListSet::npos ? _actions[fieldIdx].type() : _defActionType;
        return {fieldIdx, type};
    }
    inline std::pair<value::TypeTags, value::Value> value() const {
        return _objRoot->getAt(_idx);
    }
    inline void appendTo(UniqueBSONObjBuilder& bob) const {
        auto [tag, val] = value();
        bson::appendValueToBsonObj(bob, fieldName(), tag, val);
    }

private:
    const StringListSet& _fields;
    const std::vector<MakeObjSpec::FieldAction>& _actions;
    MakeObjSpec::ActionType _defActionType;

    value::Object* _objRoot{nullptr};
    size_t _idx{0};
    size_t _endIdx{0};
};

}  // namespace mongo::sbe::vm
