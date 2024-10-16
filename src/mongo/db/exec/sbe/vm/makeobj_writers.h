/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

#if defined(_MSC_VER)
#define MONGO_COMPILER_ALWAYS_INLINE_WITH_INLINE_SPEC MONGO_COMPILER_ALWAYS_INLINE
#else
#define MONGO_COMPILER_ALWAYS_INLINE_WITH_INLINE_SPEC MONGO_COMPILER_ALWAYS_INLINE inline
#endif

namespace mongo::sbe::vm {
class BsonObjWriter;

/**
 * MakeObj writer for outputting BSON arrays.
 */
class BsonArrWriter {
public:
    MONGO_COMPILER_ALWAYS_INLINE BsonArrWriter() {}

    MONGO_COMPILER_ALWAYS_INLINE explicit BsonArrWriter(UniqueBSONArrayBuilder bab)
        : _bab(std::move(bab)) {}

    MONGO_COMPILER_ALWAYS_INLINE void appendValue(value::TypeTags tag, value::Value val) {
        bson::appendValueToBsonArr(_bab, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE_WITH_INLINE_SPEC BsonObjWriter startObj();

    MONGO_COMPILER_ALWAYS_INLINE_WITH_INLINE_SPEC void finishObj(BsonObjWriter nestedWriter);

    MONGO_COMPILER_ALWAYS_INLINE BsonArrWriter startArr() {
        return BsonArrWriter(UniqueBSONArrayBuilder(_bab.subarrayStart()));
    }

    MONGO_COMPILER_ALWAYS_INLINE void finishArr(BsonArrWriter) {
        // Do nothing. The BsonArrWriter class's destructor will perform any necessary finishing
        // steps.
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> done() {
        _bab.doneFast();
        char* data = _bab.bb().release().release();
        return {value::TypeTags::bsonArray, value::bitcastFrom<char*>(data)};
    }

private:
    friend class BsonObjWriter;

    UniqueBSONArrayBuilder _bab;
};

/**
 * MakeObj writer for outputting BSON objects.
 */
class BsonObjWriter {
public:
    MONGO_COMPILER_ALWAYS_INLINE BsonObjWriter() {}

    MONGO_COMPILER_ALWAYS_INLINE explicit BsonObjWriter(UniqueBSONObjBuilder bob)
        : _bob(std::move(bob)) {}

    MONGO_COMPILER_ALWAYS_INLINE void appendValue(StringData fieldName,
                                                  value::TypeTags tag,
                                                  value::Value val) {
        bson::appendValueToBsonObj(_bob, fieldName, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE void appendBsonElement(const BSONElement& bsonElement) {
        _bob.append(bsonElement);
    }

    MONGO_COMPILER_ALWAYS_INLINE BsonObjWriter startObj(StringData fieldName) {
        return BsonObjWriter(UniqueBSONObjBuilder(_bob.subobjStart(fieldName)));
    }

    MONGO_COMPILER_ALWAYS_INLINE void finishObj(StringData, BsonObjWriter) {
        // Do nothing. The BsonObjWriter class's destructor will perform any necessary finishing
        // steps.
    }

    MONGO_COMPILER_ALWAYS_INLINE BsonArrWriter startArr(StringData fieldName) {
        return BsonArrWriter(UniqueBSONArrayBuilder(_bob.subarrayStart(fieldName)));
    }

    MONGO_COMPILER_ALWAYS_INLINE void finishArr(StringData, BsonArrWriter) {
        // Do nothing. The BsonArrWriter class's destructor will perform any necessary finishing
        // steps.
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> done() {
        _bob.doneFast();
        char* data = _bob.bb().release().release();
        return {value::TypeTags::bsonObject, value::bitcastFrom<char*>(data)};
    }

private:
    friend class BsonArrWriter;

    UniqueBSONObjBuilder _bob;
};

inline BsonObjWriter BsonArrWriter::startObj() {
    return BsonObjWriter(UniqueBSONObjBuilder(_bab.subobjStart()));
}

inline void BsonArrWriter::finishObj(BsonObjWriter) {
    // Do nothing. The BsonObjWriter class's destructor will perform any necessary finishing steps.
}

class ObjectWriter;

/**
 * MakeObj writer for outputting SBE arrays.
 */
class ArrayWriter {
public:
    MONGO_COMPILER_ALWAYS_INLINE ArrayWriter() {
        auto [tag, val] = value::makeNewArray();
        _arr = std::unique_ptr<value::Array>{value::bitcastTo<value::Array*>(val)};
    }

    MONGO_COMPILER_ALWAYS_INLINE void appendValue(value::TypeTags tag, value::Value val) {
        auto [copyTag, copyVal] = value::copyValue(tag, val);
        _arr->push_back(copyTag, copyVal);
    }

    MONGO_COMPILER_ALWAYS_INLINE_WITH_INLINE_SPEC ObjectWriter startObj();

    MONGO_COMPILER_ALWAYS_INLINE_WITH_INLINE_SPEC void finishObj(ObjectWriter nestedWriter);

    MONGO_COMPILER_ALWAYS_INLINE ArrayWriter startArr() {
        return ArrayWriter();
    }

    MONGO_COMPILER_ALWAYS_INLINE void finishArr(ArrayWriter nestedWriter) {
        _arr->push_back(value::TypeTags::Array,
                        value::bitcastFrom<value::Array*>(nestedWriter._arr.release()));
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> done() {
        return {value::TypeTags::Array, value::bitcastFrom<value::Array*>(_arr.release())};
    }

private:
    friend class ObjectWriter;

    std::unique_ptr<value::Array> _arr;
};

/**
 * MakeObj writer for outputting SBE objects.
 */
class ObjectWriter {
public:
    MONGO_COMPILER_ALWAYS_INLINE ObjectWriter() {
        auto [tag, val] = value::makeNewObject();
        _obj = std::unique_ptr<value::Object>{value::bitcastTo<value::Object*>(val)};
    }

    MONGO_COMPILER_ALWAYS_INLINE void appendValue(StringData fieldName,
                                                  value::TypeTags tag,
                                                  value::Value val) {
        auto [copyTag, copyVal] = value::copyValue(tag, val);
        _obj->push_back(fieldName, copyTag, copyVal);
    }

    MONGO_COMPILER_ALWAYS_INLINE ObjectWriter startObj(StringData) {
        return ObjectWriter();
    }

    MONGO_COMPILER_ALWAYS_INLINE void finishObj(StringData fieldName, ObjectWriter nestedWriter) {
        _obj->push_back(fieldName,
                        value::TypeTags::Object,
                        value::bitcastFrom<value::Object*>(nestedWriter._obj.release()));
    }

    MONGO_COMPILER_ALWAYS_INLINE ArrayWriter startArr(StringData) {
        return ArrayWriter();
    }

    MONGO_COMPILER_ALWAYS_INLINE void finishArr(StringData fieldName, ArrayWriter nestedWriter) {
        _obj->push_back(fieldName,
                        value::TypeTags::Array,
                        value::bitcastFrom<value::Array*>(nestedWriter._arr.release()));
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> done() {
        return {value::TypeTags::Object, value::bitcastFrom<value::Object*>(_obj.release())};
    }

private:
    friend class ArrayWriter;

    std::unique_ptr<value::Object> _obj;
};

inline ObjectWriter ArrayWriter::startObj() {
    return ObjectWriter();
}

inline void ArrayWriter::finishObj(ObjectWriter nestedWriter) {
    _arr->push_back(value::TypeTags::Object,
                    value::bitcastFrom<value::Object*>(nestedWriter._obj.release()));
}
}  // namespace mongo::sbe::vm
