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

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/makeobj_writers.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/hash/hash.h>
#include <boost/optional/optional.hpp>

namespace mongo::sbe::vm {

// MakeObj input cursor for BSON objects.
class BsonObjCursor {
public:
    MONGO_COMPILER_ALWAYS_INLINE BsonObjCursor(const char* be) : _be(be) {
        _last = _be + ConstDataView(_be).read<LittleEndian<uint32_t>>() - 1;
        _be += 4;
        if (_be != _last) {
            // Initialize '_name' and '_nextBe'.
            _name = bson::fieldNameAndLength(_be);
            _nextBe = bson::advance(_be, _name.size());
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE bool atEnd() const {
        return _be == _last;
    }
    MONGO_COMPILER_ALWAYS_INLINE void moveNext() {
        _be = _nextBe;
        if (_be != _last) {
            // Update '_name' and '_nextBe'.
            _name = bson::fieldNameAndLength(_be);
            _nextBe = bson::advance(_be, _name.size());
        }
    }
    MONGO_COMPILER_ALWAYS_INLINE StringData fieldName() const {
        return _name;
    }
    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> value() const {
        return bson::convertFrom<true>(bsonElement());
    }
    MONGO_COMPILER_ALWAYS_INLINE void appendTo(BsonObjWriter& bob) const {
        bob.appendBsonElement(bsonElement());
    }
    MONGO_COMPILER_ALWAYS_INLINE void appendTo(ObjectWriter& bob) const {
        auto [tag, val] = value();
        bob.appendValue(fieldName(), tag, val);
    }

private:
    MONGO_COMPILER_ALWAYS_INLINE BSONElement bsonElement() const {
        auto fieldNameLenWithNull = _name.size() + 1;
        return BSONElement(_be, fieldNameLenWithNull, BSONElement::TrustedInitTag{});
    }

    const char* _be{nullptr};
    const char* _nextBe{nullptr};
    const char* _last{nullptr};

    StringData _name;
};

// MakeObj input cursor for SBE objects.
class ObjectCursor {
public:
    MONGO_COMPILER_ALWAYS_INLINE ObjectCursor(value::Object* objRoot)
        : _objRoot(objRoot), _idx(0), _endIdx(_objRoot->size()) {
        if (_idx != _endIdx) {
            // Initialize '_name'.
            _name = StringData(_objRoot->field(_idx));
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE bool atEnd() const {
        return _idx == _endIdx;
    }
    MONGO_COMPILER_ALWAYS_INLINE void moveNext() {
        ++_idx;
        if (_idx != _endIdx) {
            // Update '_name'.
            _name = StringData(_objRoot->field(_idx));
        }
    }
    MONGO_COMPILER_ALWAYS_INLINE StringData fieldName() const {
        return _name;
    }
    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> value() const {
        return _objRoot->getAt(_idx);
    }
    MONGO_COMPILER_ALWAYS_INLINE void appendTo(BsonObjWriter& bob) const {
        auto [tag, val] = value();
        bob.appendValue(_name, tag, val);
    }
    MONGO_COMPILER_ALWAYS_INLINE void appendTo(ObjectWriter& bob) const {
        auto [tag, val] = value();
        bob.appendValue(_name, tag, val);
    }

private:
    value::Object* _objRoot{nullptr};
    size_t _idx{0};
    size_t _endIdx{0};

    StringData _name;
};

}  // namespace mongo::sbe::vm
