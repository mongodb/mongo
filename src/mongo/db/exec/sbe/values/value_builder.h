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
#include "mongo/db/storage/key_string.h"
#include "mongo/util/bufreader.h"

namespace mongo::sbe::value {

/**
 * A ValueBuilder can be used as a stream input (with a << operator), like a BSONObjBuilder. Instead
 * of converting its inputs to BSON, it converts them to pairs of sbe::value::TypeTags and
 * sbe::value::Value. During construction, these pairs are stored in the parallel '_tagList' and
 * '_valList' arrays, as a "structure of arrays."
 *
 * After constructing the array, an implementer of ValueBuilder must provide a 'readValues()' method
 * to populate the tags/vals into a container or an  sbe SlotAccessor. Some "views" (values that are
 * pointers into other memory) are constructed by appending them to the 'valueBufferBuilder'
 * provided to the constructor, and the internal buffer in that 'valueBufferBuilder' must be kept
 * alive for as long as the accessors are to remain valid.
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
 */
class ValueBuilder {
public:
    ValueBuilder(BufBuilder* valueBufferBuilder) : _valueBufferBuilder(valueBufferBuilder) {}
    ValueBuilder(ValueBuilder& other) = delete;

    ~ValueBuilder() = default;

    void append(const MinKeyLabeler& id) {
        appendValue(TypeTags::MinKey, 0);
    }

    void append(const MaxKeyLabeler& id) {
        appendValue(TypeTags::MaxKey, 0);
    }

    void append(const NullLabeler& id) {
        appendValue(TypeTags::Null, 0);
    }

    void append(const UndefinedLabeler& id) {
        appendValue(TypeTags::bsonUndefined, 0);
    }

    void append(const bool in) {
        appendValue(TypeTags::Boolean, value::bitcastFrom<bool>(in));
    }

    void append(const Date_t& in) {
        appendValue(TypeTags::Date, value::bitcastFrom<int64_t>(in.toMillisSinceEpoch()));
    }

    void append(const Timestamp& in) {
        appendValue(TypeTags::Timestamp, value::bitcastFrom<uint64_t>(in.asULL()));
    }

    void append(const OID& in) {
        appendValueBufferOffset(TypeTags::ObjectId);
        _valueBufferBuilder->appendBuf(in.view().view(), OID::kOIDSize);
    }

    void append(const std::string& in) {
        append(StringData{in});
    }

    void append(StringData in) {
        if (canUseSmallString({in.rawData(), in.size()})) {
            appendValue(makeSmallString({in.rawData(), in.size()}));
        } else {
            appendValueBufferOffset(TypeTags::StringBig);
            _valueBufferBuilder->appendNum(static_cast<int32_t>(in.size() + 1));
            _valueBufferBuilder->appendStr(in, true /* includeEndingNull */);
        }
    }

    void append(const BSONSymbol& in) {
        appendValueBufferOffset(TypeTags::bsonSymbol);
        _valueBufferBuilder->appendNum(static_cast<int32_t>(in.symbol.size() + 1));
        _valueBufferBuilder->appendStr(in.symbol, true /* includeEndingNull */);
    }

    void append(const BSONCode& in) {
        appendValueBufferOffset(TypeTags::bsonJavascript);
        // Add one to account null byte at the end.
        _valueBufferBuilder->appendNum(static_cast<uint32_t>(in.code.size() + 1));
        _valueBufferBuilder->appendStr(in.code, true /* includeEndingNull */);
    }

    void append(const BSONCodeWScope& in) {
        appendValueBufferOffset(TypeTags::bsonCodeWScope);
        _valueBufferBuilder->appendNum(
            static_cast<uint32_t>(4 + in.code.size() + 1 + in.scope.objsize()));
        _valueBufferBuilder->appendNum(static_cast<int32_t>(in.code.size() + 1));
        _valueBufferBuilder->appendStr(in.code, true /* includeEndingNull */);
        _valueBufferBuilder->appendBuf(in.scope.objdata(), in.scope.objsize());
    }

    void append(const BSONBinData& in) {
        appendValueBufferOffset(TypeTags::bsonBinData);
        _valueBufferBuilder->appendNum(in.length);
        _valueBufferBuilder->appendNum(static_cast<char>(in.type));
        _valueBufferBuilder->appendBuf(in.data, in.length);
    }

    void append(const BSONRegEx& in) {
        appendValueBufferOffset(TypeTags::bsonRegex);
        _valueBufferBuilder->appendStr(in.pattern, true /* includeEndingNull */);
        _valueBufferBuilder->appendStr(in.flags, true /* includeEndingNull */);
    }

    void append(const BSONDBRef& in) {
        appendValueBufferOffset(TypeTags::bsonDBPointer);
        _valueBufferBuilder->appendNum(static_cast<int32_t>(in.ns.size() + 1));
        _valueBufferBuilder->appendStr(in.ns, true /* includeEndingNull */);
        _valueBufferBuilder->appendBuf(in.oid.view().view(), OID::kOIDSize);
    }

    void append(double in) {
        appendValue(TypeTags::NumberDouble, value::bitcastFrom<double>(in));
    }

    void append(const Decimal128& in) {
        appendValueBufferOffset(TypeTags::NumberDecimal);
        _valueBufferBuilder->appendNum(in);
    }

    void append(long long in) {
        appendValue(TypeTags::NumberInt64, value::bitcastFrom<int64_t>(in));
    }

    void append(int32_t in) {
        appendValue(TypeTags::NumberInt32, value::bitcastFrom<int32_t>(in));
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
     * Returns the number of sbe tag/value pairs appended to this ValueBuilder.
     */
    virtual size_t numValues() const = 0;

protected:
    std::pair<TypeTags, Value> getValue(size_t index, int bufferLen) {
        invariant(index < _numValues);
        auto tag = _tagList[index];
        auto val = _valList[index];

        switch (tag) {
            // As noted in the comments for the 'appendValueBufferOffset' function, some values
            // are stored as offsets into the buffer during construction. This is where we
            // convert those offsets into pointers.
            case TypeTags::ObjectId:
            case TypeTags::StringBig:
            case TypeTags::bsonSymbol:
            case TypeTags::NumberDecimal:
            case TypeTags::bsonObject:
            case TypeTags::bsonArray:
            case TypeTags::bsonBinData:
            case TypeTags::bsonRegex:
            case TypeTags::bsonJavascript:
            case TypeTags::bsonDBPointer:
            case TypeTags::bsonCodeWScope: {
                auto offset = bitcastTo<decltype(bufferLen)>(val);
                invariant(offset < bufferLen);
                val = bitcastFrom<const char*>(_valueBufferBuilder->buf() + offset);
                break;
            }
            default:
                // 'val' is already set correctly.
                break;
        }
        return {tag, val};
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
    // offset to pointer occurs as part of the 'readValues()' function.
    void appendValueBufferOffset(TypeTags tag) {
        _tagList[_numValues] = tag;
        _valList[_numValues] = value::bitcastFrom<int32_t>(_valueBufferBuilder->len());
        ++_numValues;
    }

    std::array<TypeTags, Ordering::kMaxCompoundIndexKeys> _tagList;
    std::array<Value, Ordering::kMaxCompoundIndexKeys> _valList;
    size_t _numValues = 0;

    BufBuilder* _valueBufferBuilder;
};

/**
 * Allows sbe tag/values to be read into a vector of OwnedValueAccessors.
 */
class OwnedValueAccessorValueBuilder : public ValueBuilder {
public:
    OwnedValueAccessorValueBuilder(BufBuilder* valueBufferBuilder, bool fromKeyString = false)
        : ValueBuilder(valueBufferBuilder) {}
    OwnedValueAccessorValueBuilder(OwnedValueAccessorValueBuilder& other) = delete;

    /*
     * Remove the last value that was streamed to this ValueBuilder.
     */
    void popValue() {
        // If the removed value was a view of a string, object or array in the '_valueBufferBuilder'
        // buffer, this value will remain in that buffer, even though we've removed it from the
        // list. It will still get deallocated along with everything else when that buffer gets
        // cleared or deleted, though, so there is no leak.
        --_numValues;
    }

    size_t numValues() const override {
        return _numValues;
    }

    /**
     * Populate the given list of accessors with TypeTags and Values. Some Values may be "views"
     * into the memory constructed by the '_valueBufferBuilder' object, which is a caller-owned
     * object that must remain valid for as long as these accessors are to remain valid.
     */
    void readValues(std::vector<OwnedValueAccessor>* accessors) {
        auto bufferLen = _valueBufferBuilder->len();
        for (size_t i = 0; i < _numValues; ++i) {
            auto [tag, val] = getValue(i, bufferLen);
            invariant(i < accessors->size());
            (*accessors)[i].reset(false, tag, val);
        }
    }
};

/**
 * A ValueBuilder that supports reading of sbe tag/values into a MaterializedRow.
 */
class MaterializedRowValueBuilder : public ValueBuilder {
public:
    MaterializedRowValueBuilder(BufBuilder* valueBufferBuilder)
        : ValueBuilder(valueBufferBuilder) {}
    MaterializedRowValueBuilder(MaterializedRowValueBuilder& other) = delete;

    size_t numValues() const override {
        size_t nVals = 0;
        size_t bufIdx = 0;
        while (bufIdx < _numValues) {
            auto tag = _tagList[bufIdx];
            auto val = _valList[bufIdx];
            if (tag == TypeTags::Boolean && !bitcastTo<bool>(val)) {
                // Nothing case.
                bufIdx++;
            } else {
                // Skip the next value
                bufIdx += 2;
            }
            nVals++;
        }
        return nVals;
    }

    void readValues(MaterializedRow& row) {
        auto bufferLen = _valueBufferBuilder->len();
        size_t bufIdx = 0;
        size_t rowIdx = 0;
        while (bufIdx < _numValues) {
            invariant(rowIdx < row.size());
            auto [_, tagNothing, valNothing] = getValue(bufIdx++, bufferLen);
            tassert(6136200, "sbe tag must be 'Boolean'", tagNothing == TypeTags::Boolean);
            if (!bitcastTo<bool>(valNothing)) {
                row.reset(rowIdx++, false, TypeTags::Nothing, 0);
            } else {
                auto [owned, tag, val] = getValue(bufIdx++, bufferLen);
                row.reset(rowIdx++, owned, tag, val);
            }
        }
    }

private:
    std::tuple<bool, TypeTags, Value> getValue(size_t index, int bufferLen) {
        auto [tag, val] = ValueBuilder::getValue(index, bufferLen);
        if (tag == TypeTags::bsonBinData) {
            auto binData = getBSONBinData(tag, val);
            BufReader buf(binData, getBSONBinDataSize(tag, val));
            auto sbeTag = buf.read<TypeTags>();
            switch (sbeTag) {
                case TypeTags::bsonBinData: {
                    // Return a pointer to one byte past the sbeTag in the inner BinData.
                    return {false, TypeTags::bsonBinData, bitcastFrom<uint8_t*>(binData + 1)};
                }
                case TypeTags::ksValue: {
                    // Read the KeyString size after the 'sbeTag' byte. This gets written to the
                    // buffer in 'KeyString::Value::serialize'.
                    auto ks =
                        KeyString::Value::deserialize(buf, KeyString::Version::kLatestVersion);
                    auto [ksTag, ksVal] = makeCopyKeyString(ks);
                    return {true, ksTag, ksVal};
                }
                case TypeTags::RecordId: {
                    int64_t ridValue = buf.read<LittleEndian<int64_t>>();
                    return {false, TypeTags::RecordId, bitcastFrom<int64_t>(ridValue)};
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
        return {false, tag, val};
    }
};

template <typename T>
void operator<<(ValueBuilder& valBuilder, T operand) {
    valBuilder.append(operand);
}
}  // namespace mongo::sbe::value
