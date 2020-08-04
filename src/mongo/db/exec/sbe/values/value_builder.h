/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/slot.h"

namespace mongo::sbe::value {

/**
 * A ValueBuilder can be used as a stream input (with a << operator), like a BSONObjBuilder. Instead
 * of converting its inputs to BSON, it converts them to pairs of sbe::value::TypeTags and
 * sbe::value::Value. During construction, these pairs are stored in the parallel '_tagList' and
 * '_valList' arrays, as a "structure of arrays."
 *
 * After constructing the array, use the 'readValues()' method to populate a ViewOfValueAccessor
 * vector. Some "views" (values that are pointers into other memory) are constructed by appending
 * them to the 'valueBufferBuilder' provided to the constructor, and the internal buffer in that
 * 'valueBufferBuilder' must be kept alive for as long as the accessors are to remain valid.
 *
 * Note that, in addition to destroying the 'valueBufferBuilder' or calling its 'reset()' or
 * 'release()' function, appending more values to the buffer (either directly or via this
 * ValueBuilder) can invalidate the underlying buffer memory.
 *
 * The 'valueBufferBuilder' is _not_ owned by the ValueBuilder class, so that the caller can reuse
 * it without freeing and then reallocating its memory.
 *
 * NB: The ValueBuilder is specifically intended to adapt KeyString::Value conversion, which
 * operates by appending results to a BSONObjBuilder, to instead convert to SBE values. It is not
 * intended as a general-purpose tool for populating SBE accessors, and no new code should construct
 * or use a ValueBuilder.
 *
 * Also note that some data types are not yet supported by SBE and appending them will throw a
 * query-fatal error.
 */
class ValueBuilder {
public:
    ValueBuilder(BufBuilder* valueBufferBuilder) : _valueBufferBuilder(valueBufferBuilder) {}
    ValueBuilder(ValueBuilder& other) = delete;

    void append(const MinKeyLabeler& id) {
        unsupportedType("minKey");
    }

    void append(const MaxKeyLabeler& id) {
        unsupportedType("maxKey");
    }

    void append(const NullLabeler& id) {
        appendValue(TypeTags::Null, 0);
    }

    void append(const UndefinedLabeler& id) {
        unsupportedType("undefined");
    }

    void append(const bool in) {
        appendValue(TypeTags::Boolean, value::bitcastFrom(in));
    }

    void append(const Date_t& in) {
        appendValue(TypeTags::Date, value::bitcastFrom(in.toMillisSinceEpoch()));
    }

    void append(const Timestamp& in) {
        appendValue(TypeTags::Timestamp, value::bitcastFrom(in.asLL()));
    }

    void append(const OID& in) {
        appendValueBufferOffset(TypeTags::ObjectId);
        _valueBufferBuilder->appendBuf(in.view().view(), OID::kOIDSize);
    }

    void append(const std::string& in) {
        append(StringData{in});
    }

    void append(StringData in) {
        if (in.size() < kSmallStringThreshold - 1) {
            appendValue(makeSmallString(std::string_view(in.rawData(), in.size())));
        } else {
            appendValueBufferOffset(TypeTags::StringBig);

            // Note: This _will_ write a NULL-terminated string, even if the input StringData does
            // not have a NULL terminator.
            _valueBufferBuilder->appendStr(in);
        }
    }

    void append(const BSONSymbol& in) {
        unsupportedType("symbol");
    }

    void append(const BSONCode& in) {
        unsupportedType("javascript");
    }

    void append(const BSONCodeWScope& in) {
        unsupportedType("javascriptWithScope");
    }

    void append(const BSONBinData& in) {
        appendValueBufferOffset(TypeTags::bsonBinData);
        _valueBufferBuilder->appendNum(in.length);
        _valueBufferBuilder->appendNum(static_cast<char>(in.type));
        _valueBufferBuilder->appendBuf(in.data, in.length);
    }

    void append(const BSONRegEx& in) {
        unsupportedType("regex");
    }

    void append(const BSONDBRef& in) {
        unsupportedType("dbPointer");
    }

    void append(double in) {
        appendValue(TypeTags::NumberDouble, value::bitcastFrom(in));
    }

    void append(const Decimal128& in) {
        appendValueBufferOffset(TypeTags::NumberDecimal);
        _valueBufferBuilder->appendNum(in);
    }

    void append(long long in) {
        appendValue(TypeTags::NumberInt64, value::bitcastFrom(in));
    }

    void append(int32_t in) {
        appendValue(TypeTags::NumberInt32, value::bitcastFrom(in));
    }

    BufBuilder& subobjStart() {
        appendValueBufferOffset(TypeTags::bsonObject);
        return *_valueBufferBuilder;
    }

    BufBuilder& subarrayStart() {
        appendValueBufferOffset(TypeTags::bsonArray);
        return *_valueBufferBuilder;
    }

    /**
     * Remove the last value that was streamed to this ValueBuilder.
     */
    void popValue() {
        // If the removed value was a view of a string, object or array in the '_valueBufferBuilder'
        // buffer, this value will remain in that buffer, even though we've removed it from the
        // list. It will still get deallocated along with everything else when that buffer gets
        // cleared or deleted, though, so there is no leak.
        --_numValues;
    }

    size_t numValues() const {
        return _numValues;
    }

    /**
     * Populate the given list of accessors with TypeTags and Values. Some Values may be "views"
     * into the memory constructed by the '_valueBufferBuilder' object, which is a caller-owned
     * object that must remain valid for as long as these accessors are to remain valid.
     */
    void readValues(std::vector<ViewOfValueAccessor>* accessors) {
        auto bufferLen = _valueBufferBuilder->len();
        for (size_t i = 0; i < _numValues; ++i) {
            auto tag = _tagList[i];
            auto val = _valList[i];

            switch (tag) {
                // As noted in the comments for the 'appendValueBufferOffset' function, some values
                // are stored as offsets into the buffer during construction. This is where we
                // convert those offsets into pointers.
                case TypeTags::ObjectId:
                case TypeTags::StringBig:
                case TypeTags::NumberDecimal:
                case TypeTags::bsonObject:
                case TypeTags::bsonArray:
                case TypeTags::bsonBinData: {
                    auto offset = bitcastTo<decltype(bufferLen)>(val);
                    invariant(offset < bufferLen);
                    val = bitcastFrom(_valueBufferBuilder->buf() + offset);
                    break;
                }
                default:
                    // 'val' is already set correctly.
                    break;
            }

            invariant(i < accessors->size());
            (*accessors)[i].reset(tag, val);
        }
    }

private:
    void unsupportedType(const char* typeDescription) {
        uasserted(4935100,
                  str::stream() << "SBE does not support type present in index entry: "
                                << typeDescription);
    }

    void appendValue(TypeTags tag, Value val) noexcept {
        _tagList[_numValues] = tag;
        _valList[_numValues] = val;
        ++_numValues;
    }

    void appendValue(std::pair<TypeTags, Value> in) noexcept {
        appendValue(in.first, in.second);
    }

    // For some TypeTags (e.g., StringBig), the corresponding Value is actually a pointer to the
    // value's location in memory. In the case of the ValueBuilder, that memory will be within the
    // buffer created by the '_valueBufferBuilder' object.
    //
    // During the building process, pointers into that memory can become invalidated, so instead of
    // storing a pointer, we store an _offset_ into the under-construction buffer. Translation from
    // offset to pointer occurs as part of the 'releaseValues()' function.
    void appendValueBufferOffset(TypeTags tag) {
        _tagList[_numValues] = tag;
        _valList[_numValues] = value::bitcastFrom(_valueBufferBuilder->len());
        ++_numValues;
    }

    std::array<TypeTags, Ordering::kMaxCompoundIndexKeys> _tagList;
    std::array<Value, Ordering::kMaxCompoundIndexKeys> _valList;
    size_t _numValues = 0;

    BufBuilder* _valueBufferBuilder;
};

template <typename T>
void operator<<(ValueBuilder& valBuilder, T operand) {
    valBuilder.append(operand);
}

}  // namespace mongo::sbe::value
