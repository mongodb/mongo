/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/values/value.h"

#include <pcrecpp.h>

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string.h"

namespace mongo {
namespace sbe {
namespace value {

std::pair<TypeTags, Value> makeCopyKeyString(const KeyString::Value& inKey) {
    auto k = new KeyString::Value(inKey);
    return {TypeTags::ksValue, bitcastFrom<KeyString::Value*>(k)};
}

std::pair<TypeTags, Value> makeCopyPcreRegex(const pcrecpp::RE& regex) {
    auto ownedRegexVal = sbe::value::bitcastFrom<pcrecpp::RE*>(new pcrecpp::RE(regex));
    return {TypeTags::pcreRegex, ownedRegexVal};
}

void releaseValue(TypeTags tag, Value val) noexcept {
    switch (tag) {
        case TypeTags::NumberDecimal:
            delete[] getRawPointerView(val);
            break;
        case TypeTags::Array:
            delete getArrayView(val);
            break;
        case TypeTags::ArraySet:
            delete getArraySetView(val);
            break;
        case TypeTags::Object:
            delete getObjectView(val);
            break;
        case TypeTags::StringBig:
            delete[] getBigStringView(val);
            break;
        case TypeTags::ObjectId:
            delete getObjectIdView(val);
            break;
        case TypeTags::bsonObject:
        case TypeTags::bsonArray:
        case TypeTags::bsonObjectId:
        case TypeTags::bsonBinData:
            delete[] getRawPointerView(val);
            break;
        case TypeTags::ksValue:
            delete getKeyStringView(val);
            break;
        case TypeTags::pcreRegex:
            delete getPcreRegexView(val);
            break;
        default:
            break;
    }
}

template <typename T>
void writeTagToStream(T& stream, const TypeTags tag) {
    switch (tag) {
        case TypeTags::Nothing:
            stream << "Nothing";
            break;
        case TypeTags::NumberInt32:
            stream << "NumberInt32";
            break;
        case TypeTags::NumberInt64:
            stream << "NumberInt64";
            break;
        case TypeTags::NumberDouble:
            stream << "NumberDouble";
            break;
        case TypeTags::NumberDecimal:
            stream << "NumberDecimal";
            break;
        case TypeTags::Date:
            stream << "Date";
            break;
        case TypeTags::Timestamp:
            stream << "Timestamp";
            break;
        case TypeTags::Boolean:
            stream << "Boolean";
            break;
        case TypeTags::Null:
            stream << "Null";
            break;
        case TypeTags::StringSmall:
            stream << "StringSmall";
            break;
        case TypeTags::StringBig:
            stream << "StringBig";
            break;
        case TypeTags::Array:
            stream << "Array";
            break;
        case TypeTags::ArraySet:
            stream << "ArraySet";
            break;
        case TypeTags::Object:
            stream << "Object";
            break;
        case TypeTags::ObjectId:
            stream << "ObjectId";
            break;
        case TypeTags::bsonObject:
            stream << "bsonObject";
            break;
        case TypeTags::bsonArray:
            stream << "bsonArray";
            break;
        case TypeTags::bsonString:
            stream << "bsonString";
            break;
        case TypeTags::bsonObjectId:
            stream << "bsonObjectId";
            break;
        case TypeTags::bsonBinData:
            stream << "bsonBinData";
            break;
        case TypeTags::ksValue:
            stream << "KeyString";
            break;
        case TypeTags::pcreRegex:
            stream << "pcreRegex";
            break;
        case TypeTags::timeZoneDB:
            stream << "timeZoneDB";
            break;
        default:
            stream << "unknown tag";
            break;
    }
}

template <typename T>
void writeValueToStream(T& stream, TypeTags tag, Value val) {
    switch (tag) {
        case value::TypeTags::NumberInt32:
            stream << bitcastTo<int32_t>(val);
            break;
        case value::TypeTags::NumberInt64:
            stream << bitcastTo<int64_t>(val);
            break;
        case value::TypeTags::NumberDouble:
            stream << bitcastTo<double>(val);
            break;
        case value::TypeTags::NumberDecimal:
            stream << bitcastTo<Decimal128>(val).toString();
            break;
        case value::TypeTags::Date:
            stream << bitcastTo<int64_t>(val);
            break;
        case value::TypeTags::Boolean:
            stream << (bitcastTo<bool>(val) ? "true" : "false");
            break;
        case value::TypeTags::Null:
            stream << "null";
            break;
        case value::TypeTags::StringSmall:
            stream << '"' << getSmallStringView(val) << '"';
            break;
        case value::TypeTags::StringBig:
            stream << '"' << getBigStringView(val) << '"';
            break;
        case value::TypeTags::Array: {
            auto arr = getArrayView(val);
            stream << '[';
            for (size_t idx = 0; idx < arr->size(); ++idx) {
                if (idx != 0) {
                    stream << ", ";
                }
                auto [tag, val] = arr->getAt(idx);
                writeValueToStream(stream, tag, val);
            }
            stream << ']';
            break;
        }
        case value::TypeTags::ArraySet: {
            auto arr = getArraySetView(val);
            stream << '[';
            bool first = true;
            for (const auto& v : arr->values()) {
                if (!first) {
                    stream << ", ";
                }
                first = false;
                writeValueToStream(stream, v.first, v.second);
            }
            stream << ']';
            break;
        }
        case value::TypeTags::Object: {
            auto obj = getObjectView(val);
            stream << '{';
            for (size_t idx = 0; idx < obj->size(); ++idx) {
                if (idx != 0) {
                    stream << ", ";
                }
                stream << '"' << obj->field(idx) << '"';
                stream << " : ";
                auto [tag, val] = obj->getAt(idx);
                writeValueToStream(stream, tag, val);
            }
            stream << '}';
            break;
        }
        case value::TypeTags::ObjectId: {
            auto objId = getObjectIdView(val);
            stream << "ObjectId(\"" << OID::from(objId->data()).toString() << "\")";
            break;
        }
        case value::TypeTags::Nothing:
            stream << "---===*** NOTHING ***===---";
            break;
        case value::TypeTags::bsonArray: {
            const char* be = getRawPointerView(val);
            const char* end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
            bool first = true;
            // Skip document length.
            be += 4;
            stream << '[';
            while (*be != 0) {
                auto sv = bson::fieldNameView(be);

                if (!first) {
                    stream << ", ";
                }
                first = false;

                auto [tag, val] = bson::convertFrom(true, be, end, sv.size());
                writeValueToStream(stream, tag, val);

                be = bson::advance(be, sv.size());
            }
            stream << ']';
            break;
        }
        case value::TypeTags::bsonObject: {
            const char* be = getRawPointerView(val);
            const char* end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
            bool first = true;
            // Skip document length.
            be += 4;
            stream << '{';
            while (*be != 0) {
                auto sv = bson::fieldNameView(be);

                if (!first) {
                    stream << ", ";
                }
                first = false;

                stream << '"' << std::string(sv) << '"';
                stream << " : ";
                auto [tag, val] = bson::convertFrom(true, be, end, sv.size());
                writeValueToStream(stream, tag, val);

                be = bson::advance(be, sv.size());
            }
            stream << '}';
            break;
        }
        case value::TypeTags::bsonString:
            stream << '"' << std::string(getStringView(value::TypeTags::bsonString, val)) << '"';
            break;
        case value::TypeTags::bsonObjectId:
            stream << "---===*** bsonObjectId ***===---";
            break;
        case value::TypeTags::bsonBinData:
            stream << "---===*** bsonBinData ***===---";
            break;
        case value::TypeTags::ksValue: {
            auto ks = getKeyStringView(val);
            stream << "KS(" << ks->toString() << ")";
            break;
        }
        case value::TypeTags::Timestamp: {
            Timestamp ts{bitcastTo<uint64_t>(val)};
            stream << ts.toString();
            break;
        }
        case value::TypeTags::pcreRegex: {
            auto regex = getPcreRegexView(val);
            // TODO: Also include the regex flags.
            stream << "/" << regex->pattern() << "/";
            break;
        }
        case value::TypeTags::timeZoneDB: {
            auto tzdb = getTimeZoneDBView(val);
            auto timeZones = tzdb->getTimeZoneStrings();
            stream << "TimeZoneDatabase(" + timeZones.front() + "..." + timeZones.back() + ")";
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

std::ostream& operator<<(std::ostream& os, const TypeTags tag) {
    writeTagToStream(os, tag);
    return os;
}

str::stream& operator<<(str::stream& str, const TypeTags tag) {
    writeTagToStream(str, tag);
    return str;
}

std::ostream& operator<<(std::ostream& os, const std::pair<TypeTags, Value>& value) {
    writeValueToStream(os, value.first, value.second);
    return os;
}

str::stream& operator<<(str::stream& str, const std::pair<TypeTags, Value>& value) {
    writeValueToStream(str, value.first, value.second);
    return str;
}

BSONType tagToType(TypeTags tag) noexcept {
    switch (tag) {
        case TypeTags::Nothing:
            return BSONType::EOO;
        case TypeTags::NumberInt32:
            return BSONType::NumberInt;
        case TypeTags::NumberInt64:
            return BSONType::NumberLong;
        case TypeTags::NumberDouble:
            return BSONType::NumberDouble;
        case TypeTags::NumberDecimal:
            return BSONType::NumberDecimal;
        case TypeTags::Date:
            return BSONType::Date;
        case TypeTags::Timestamp:
            return BSONType::bsonTimestamp;
        case TypeTags::Boolean:
            return BSONType::Bool;
        case TypeTags::Null:
            return BSONType::jstNULL;
        case TypeTags::StringSmall:
            return BSONType::String;
        case TypeTags::StringBig:
            return BSONType::String;
        case TypeTags::Array:
            return BSONType::Array;
        case TypeTags::ArraySet:
            return BSONType::Array;
        case TypeTags::Object:
            return BSONType::Object;
        case TypeTags::ObjectId:
            return BSONType::jstOID;
        case TypeTags::bsonObject:
            return BSONType::Object;
        case TypeTags::bsonArray:
            return BSONType::Array;
        case TypeTags::bsonString:
            return BSONType::String;
        case TypeTags::bsonObjectId:
            return BSONType::jstOID;
        case TypeTags::bsonBinData:
            return BSONType::BinData;
        case TypeTags::ksValue:
            // This is completely arbitrary.
            return BSONType::EOO;
        default:
            MONGO_UNREACHABLE;
    }
}

std::size_t hashValue(TypeTags tag, Value val) noexcept {
    switch (tag) {
        case TypeTags::NumberInt32:
            return absl::Hash<int32_t>{}(bitcastTo<int32_t>(val));
        case TypeTags::NumberInt64:
            return absl::Hash<int64_t>{}(bitcastTo<int64_t>(val));
        case TypeTags::NumberDouble:
            // Force doubles to integers for hashing.
            return absl::Hash<int64_t>{}(bitcastTo<double>(val));
        case TypeTags::NumberDecimal:
            // Force decimals to integers for hashing.
            return absl::Hash<int64_t>{}(bitcastTo<Decimal128>(val).toLong());
        case TypeTags::Date:
            return absl::Hash<int64_t>{}(bitcastTo<int64_t>(val));
        case TypeTags::Timestamp:
            return absl::Hash<uint64_t>{}(bitcastTo<uint64_t>(val));
        case TypeTags::Boolean:
            return bitcastTo<bool>(val);
        case TypeTags::Null:
            return 0;
        case TypeTags::StringSmall:
        case TypeTags::StringBig:
        case TypeTags::bsonString: {
            auto sv = getStringView(tag, val);
            return absl::Hash<std::string_view>{}(sv);
        }
        case TypeTags::ObjectId: {
            auto id = getObjectIdView(val);
            return absl::Hash<uint64_t>{}(readFromMemory<uint64_t>(id->data())) ^
                absl::Hash<uint32_t>{}(readFromMemory<uint32_t>(id->data() + 8));
        }
        case TypeTags::ksValue: {
            return getKeyStringView(val)->hash();
        }
        case TypeTags::Array:
        case TypeTags::ArraySet:
        case TypeTags::bsonArray: {
            auto arr = ArrayEnumerator{tag, val};
            auto res = hashInit();

            // There should be enough entropy in the first 4 elements.
            for (int i = 0; i < 4 && !arr.atEnd(); ++i) {
                auto [elemTag, elemVal] = arr.getViewOfValue();
                res = hashCombine(res, hashValue(elemTag, elemVal));
                arr.advance();
            }

            return res;
        }
        case TypeTags::Object:
        case TypeTags::bsonObject: {
            auto obj = ObjectEnumerator{tag, val};
            auto res = hashInit();

            // There should be enough entropy in the first 4 elements.
            for (int i = 0; i < 4 && !obj.atEnd(); ++i) {
                auto [elemTag, elemVal] = obj.getViewOfValue();
                res = hashCombine(res, hashValue(elemTag, elemVal));
                obj.advance();
            }

            return res;
        }
        case TypeTags::bsonBinData: {
            auto size = getBSONBinDataSize(tag, val);
            if (size < 8) {
                // Zero initialize buffer and copy bytes in.
                char buffer[8] = {};
                memcpy(buffer, getRawPointerView(val), size);

                // Hash as if it is 64bit integer.
                return absl::Hash<uint64_t>{}(readFromMemory<uint64_t>(buffer));
            } else {
                // Hash only the first 8 bytes. It should be enough.
                return absl::Hash<uint64_t>{}(
                    readFromMemory<uint64_t>(getRawPointerView(val) + sizeof(uint32_t)));
            }
        }
        default:
            break;
    }

    return 0;
}


/**
 * Performs a three-way comparison for any type that has < and == operators. Additionally,
 * guarantees that the result will be exactlty -1, 0, or 1, which is important, because not all
 * comparison functions make that guarantee.
 *
 * The std::string_view::compare(basic_string_view s) function, for example, only promises that it
 * will return a value less than 0 in the case that 'this' is less than 's,' whereas we want to
 * return exactly -1.
 */
template <typename T>
int32_t compareHelper(const T lhs, const T rhs) noexcept {
    return lhs < rhs ? -1 : (lhs == rhs ? 0 : 1);
}

/*
 * Three ways value comparison (aka spacehip operator).
 */
std::pair<TypeTags, Value> compareValue(TypeTags lhsTag,
                                        Value lhsValue,
                                        TypeTags rhsTag,
                                        Value rhsValue) {
    if (isNumber(lhsTag) && isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case TypeTags::NumberInt32: {
                auto result = compareHelper(numericCast<int32_t>(lhsTag, lhsValue),
                                            numericCast<int32_t>(rhsTag, rhsValue));
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
            }
            case TypeTags::NumberInt64: {
                auto result = compareHelper(numericCast<int64_t>(lhsTag, lhsValue),
                                            numericCast<int64_t>(rhsTag, rhsValue));
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
            }
            case TypeTags::NumberDouble: {
                auto result = compareHelper(numericCast<double>(lhsTag, lhsValue),
                                            numericCast<double>(rhsTag, rhsValue));
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
            }
            case TypeTags::NumberDecimal: {
                auto result = compareHelper(numericCast<Decimal128>(lhsTag, lhsValue),
                                            numericCast<Decimal128>(rhsTag, rhsValue));
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (isString(lhsTag) && isString(rhsTag)) {
        auto lhsStr = getStringView(lhsTag, lhsValue);
        auto rhsStr = getStringView(rhsTag, rhsValue);
        auto result = lhsStr.compare(rhsStr);
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    } else if (lhsTag == TypeTags::Date && rhsTag == TypeTags::Date) {
        auto result = compareHelper(bitcastTo<int64_t>(lhsValue), bitcastTo<int64_t>(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
    } else if (lhsTag == TypeTags::Timestamp && rhsTag == TypeTags::Timestamp) {
        auto result = compareHelper(bitcastTo<uint64_t>(lhsValue), bitcastTo<uint64_t>(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
    } else if (lhsTag == TypeTags::Boolean && rhsTag == TypeTags::Boolean) {
        auto result = compareHelper(bitcastTo<bool>(lhsValue), bitcastTo<bool>(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
    } else if (lhsTag == TypeTags::Null && rhsTag == TypeTags::Null) {
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else if (isArray(lhsTag) && isArray(rhsTag)) {
        auto lhsArr = ArrayEnumerator{lhsTag, lhsValue};
        auto rhsArr = ArrayEnumerator{rhsTag, rhsValue};
        while (!lhsArr.atEnd() && !rhsArr.atEnd()) {
            auto [lhsTag, lhsVal] = lhsArr.getViewOfValue();
            auto [rhsTag, rhsVal] = rhsArr.getViewOfValue();

            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
            if (tag != TypeTags::NumberInt32 || bitcastTo<int32_t>(val) != 0) {
                return {tag, val};
            }
            lhsArr.advance();
            rhsArr.advance();
        }
        if (lhsArr.atEnd() && rhsArr.atEnd()) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
        } else if (lhsArr.atEnd()) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(-1)};
        } else {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(1)};
        }
    } else if (isObject(lhsTag) && isObject(rhsTag)) {
        auto lhsObj = ObjectEnumerator{lhsTag, lhsValue};
        auto rhsObj = ObjectEnumerator{rhsTag, rhsValue};
        while (!lhsObj.atEnd() && !rhsObj.atEnd()) {
            auto fieldCmp = lhsObj.getFieldName().compare(rhsObj.getFieldName());
            if (fieldCmp != 0) {
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(fieldCmp, 0))};
            }

            auto [lhsTag, lhsVal] = lhsObj.getViewOfValue();
            auto [rhsTag, rhsVal] = rhsObj.getViewOfValue();

            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
            if (tag != TypeTags::NumberInt32 || bitcastTo<int32_t>(val) != 0) {
                return {tag, val};
            }
            lhsObj.advance();
            rhsObj.advance();
        }
        if (lhsObj.atEnd() && rhsObj.atEnd()) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
        } else if (lhsObj.atEnd()) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(-1)};
        } else {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(1)};
        }
    } else if (isObjectId(lhsTag) && isObjectId(rhsTag)) {
        auto lhsObjId = lhsTag == TypeTags::ObjectId ? getObjectIdView(lhsValue)->data()
                                                     : bitcastTo<uint8_t*>(lhsValue);
        auto rhsObjId = rhsTag == TypeTags::ObjectId ? getObjectIdView(rhsValue)->data()
                                                     : bitcastTo<uint8_t*>(rhsValue);
        auto result = memcmp(lhsObjId, rhsObjId, sizeof(ObjectIdType));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    } else if (isBinData(lhsTag) && isBinData(rhsTag)) {
        auto lsz = getBSONBinDataSize(lhsTag, lhsValue);
        auto rsz = getBSONBinDataSize(rhsTag, rhsValue);
        if (lsz != rsz) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(lsz, rsz))};
        }

        // Since we already compared the size above, skip the first 4 bytes of the buffer and
        // compare the lsz+1 bytes carrying the subtype and binData payload in one pass.
        auto result = memcmp(getRawPointerView(lhsValue) + sizeof(uint32_t),
                             getRawPointerView(rhsValue) + sizeof(uint32_t),
                             lsz + 1);
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    } else if (lhsTag == TypeTags::ksValue && rhsTag == TypeTags::ksValue) {
        auto result = getKeyStringView(lhsValue)->compare(*getKeyStringView(lhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
    } else if (lhsTag == TypeTags::Nothing && rhsTag == TypeTags::Nothing) {
        // Special case for Nothing in a hash table (group) and sort comparison.
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else {
        // Different types.
        auto result =
            canonicalizeBSONType(tagToType(lhsTag)) - canonicalizeBSONType(tagToType(rhsTag));
        invariant(result != 0);
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    }
}

bool isNaN(TypeTags tag, Value val) {
    return (tag == TypeTags::NumberDouble && std::isnan(bitcastTo<double>(val))) ||
        (tag == TypeTags::NumberDecimal && bitcastTo<Decimal128>(val).isNaN());
}

void ArraySet::push_back(TypeTags tag, Value val) {
    if (tag != TypeTags::Nothing) {
        ValueGuard guard{tag, val};
        auto [it, inserted] = _values.insert({tag, val});

        if (inserted) {
            guard.reset();
        }
    }
}


std::pair<TypeTags, Value> ArrayEnumerator::getViewOfValue() const {
    if (_array) {
        return _array->getAt(_index);
    } else if (_arraySet) {
        return {_iter->first, _iter->second};
    } else {
        auto sv = bson::fieldNameView(_arrayCurrent);
        return bson::convertFrom(true, _arrayCurrent, _arrayEnd, sv.size());
    }
}

bool ArrayEnumerator::advance() {
    if (_array) {
        if (_index < _array->size()) {
            ++_index;
        }

        return _index < _array->size();
    } else if (_arraySet) {
        if (_iter != _arraySet->values().end()) {
            ++_iter;
        }

        return _iter != _arraySet->values().end();
    } else {
        if (*_arrayCurrent != 0) {
            auto sv = bson::fieldNameView(_arrayCurrent);
            _arrayCurrent = bson::advance(_arrayCurrent, sv.size());
        }

        return *_arrayCurrent != 0;
    }
}

std::pair<TypeTags, Value> ObjectEnumerator::getViewOfValue() const {
    if (_object) {
        return _object->getAt(_index);
    } else {
        auto sv = bson::fieldNameView(_objectCurrent);
        return bson::convertFrom(true, _objectCurrent, _objectEnd, sv.size());
    }
}

bool ObjectEnumerator::advance() {
    if (_object) {
        if (_index < _object->size()) {
            ++_index;
        }

        return _index < _object->size();
    } else {
        if (*_objectCurrent != 0) {
            auto sv = bson::fieldNameView(_objectCurrent);
            _objectCurrent = bson::advance(_objectCurrent, sv.size());
        }

        return *_objectCurrent != 0;
    }
}

std::string_view ObjectEnumerator::getFieldName() const {
    using namespace std::literals;
    if (_object) {
        if (_index < _object->size()) {
            return _object->field(_index);
        } else {
            return ""sv;
        }
    } else {
        if (*_objectCurrent != 0) {
            return bson::fieldNameView(_objectCurrent);
        } else {
            return ""sv;
        }
    }
}

void readKeyStringValueIntoAccessors(const KeyString::Value& keyString,
                                     const Ordering& ordering,
                                     BufBuilder* valueBufferBuilder,
                                     std::vector<ViewOfValueAccessor>* accessors,
                                     boost::optional<IndexKeysInclusionSet> indexKeysToInclude) {
    ValueBuilder valBuilder(valueBufferBuilder);
    invariant(!indexKeysToInclude || indexKeysToInclude->count() == accessors->size());

    BufReader reader(keyString.getBuffer(), keyString.getSize());
    KeyString::TypeBits typeBits(keyString.getTypeBits());
    KeyString::TypeBits::Reader typeBitsReader(typeBits);

    bool keepReading = true;
    size_t componentIndex = 0;
    do {
        // In the edge case that 'componentIndex' indicates that we have already read
        // 'kMaxCompoundIndexKeys' components, we expect that the next 'readSBEValue()' will return
        // false (to indicate EOF), so the value of 'inverted' does not matter.
        bool inverted = (componentIndex < Ordering::kMaxCompoundIndexKeys)
            ? (ordering.get(componentIndex) == -1)
            : false;

        keepReading = KeyString::readSBEValue(
            &reader, &typeBitsReader, inverted, typeBits.version, &valBuilder);

        invariant(componentIndex < Ordering::kMaxCompoundIndexKeys || !keepReading);

        // If 'indexKeysToInclude' indicates that this index key component is not part of the
        // projection, remove it from the list of values that will be fed to the 'accessors' list.
        // Note that, even when we are excluding a key component, we can't skip the call to
        // 'KeyString::readSBEValue()' because it is needed to advance the 'reader' and
        // 'typeBitsReader' stream.
        if (indexKeysToInclude && (componentIndex < Ordering::kMaxCompoundIndexKeys) &&
            !(*indexKeysToInclude)[componentIndex]) {
            valBuilder.popValue();
        }
        ++componentIndex;
    } while (keepReading && valBuilder.numValues() < accessors->size());

    valBuilder.readValues(accessors);
}

}  // namespace value
}  // namespace sbe
}  // namespace mongo
