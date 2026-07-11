// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/makeobj_writers.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

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
    MONGO_COMPILER_ALWAYS_INLINE std::string_view fieldName() const {
        return _name;
    }
    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> value() const {
        return bson::convertToView(bsonElement());
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

    std::string_view _name;
};

// MakeObj input cursor for SBE objects.
class ObjectCursor {
public:
    MONGO_COMPILER_ALWAYS_INLINE ObjectCursor(value::Object* objRoot)
        : _objRoot(objRoot), _idx(0), _endIdx(_objRoot->size()) {
        if (_idx != _endIdx) {
            // Initialize '_name'.
            _name = std::string_view(_objRoot->field(_idx));
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE bool atEnd() const {
        return _idx == _endIdx;
    }
    MONGO_COMPILER_ALWAYS_INLINE void moveNext() {
        ++_idx;
        if (_idx != _endIdx) {
            // Update '_name'.
            _name = std::string_view(_objRoot->field(_idx));
        }
    }
    MONGO_COMPILER_ALWAYS_INLINE std::string_view fieldName() const {
        return _name;
    }
    MONGO_COMPILER_ALWAYS_INLINE value::TagValueView value() const {
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

    std::string_view _name;
};

}  // namespace mongo::sbe::vm
