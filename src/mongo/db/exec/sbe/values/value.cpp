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

#include "mongo/db/exec/sbe/values/value.h"

#include "mongo/base/compare_numbers.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/generic_compare.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/duration.h"

#include <cmath>

#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#include <absl/strings/string_view.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
namespace value {
namespace {
template <typename T>
auto abslHash(const T& val) {
    if constexpr (std::is_same_v<T, StringData>) {
        return absl::Hash<absl::string_view>{}(absl::string_view{val.data(), val.size()});
    } else if constexpr (IsEndian<T>::value) {
        return abslHash(val.value);
    } else {
        return absl::Hash<T>{}(val);
    }
}
}  // namespace

constexpr size_t gTypeOpsSize =
    size_t(TypeTags::TypeTagsMax) - size_t(TypeTags::EndOfNativeTypeTags);

const ExtendedTypeOps* gTypeOps[gTypeOpsSize] = {0};

const ExtendedTypeOps* getExtendedTypeOps(TypeTags tag) {
    dassert(size_t(tag) > size_t(TypeTags::EndOfNativeTypeTags));

    size_t typeOpsIdx = size_t(tag) - (size_t(TypeTags::EndOfNativeTypeTags) + 1);
    return gTypeOps[typeOpsIdx];
}

void registerExtendedTypeOps(TypeTags tag, const ExtendedTypeOps* typeOps) {
    tassert(7690414,
            "Expected tag value to be within the range of extended types",
            size_t(tag) > size_t(TypeTags::EndOfNativeTypeTags));

    size_t typeOpsIdx = size_t(tag) - (size_t(TypeTags::EndOfNativeTypeTags) + 1);
    gTypeOps[typeOpsIdx] = typeOps;
}

std::pair<TypeTags, Value> makeNewBsonRegex(StringData pattern, StringData flags) {
    // Add 2 to account NULL bytes after pattern and flags.
    auto totalSize = pattern.size() + flags.size() + 2;
    auto buffer = std::make_unique<char[]>(totalSize);
    auto rawBuffer = buffer.get();

    // Copy pattern first and flags after it.
    memcpy(rawBuffer, pattern.data(), pattern.size());
    memcpy(rawBuffer + pattern.size() + 1, flags.data(), flags.size());

    // Ensure NULL byte is placed after each part.
    rawBuffer[pattern.size()] = '\0';
    rawBuffer[totalSize - 1] = '\0';
    return {TypeTags::bsonRegex, bitcastFrom<char*>(buffer.release())};
}

std::pair<TypeTags, Value> makeCopyBsonJavascript(StringData code) {
    auto [_, strVal] = makeBigString(code);
    return {TypeTags::bsonJavascript, strVal};
}

std::pair<TypeTags, Value> makeNewBsonDBPointer(StringData ns, const uint8_t* id) {
    const auto nsLen = ns.size();
    const auto nsLenWithNull = nsLen + sizeof(char);
    auto buffer = std::make_unique<char[]>(sizeof(uint32_t) + nsLenWithNull + sizeof(ObjectIdType));
    char* ptr = buffer.get();

    // Write length of 'ns' as a little-endian uint32_t.
    DataView(ptr).write<LittleEndian<uint32_t>>(nsLenWithNull);
    ptr += sizeof(uint32_t);

    // Write 'ns' followed by a null terminator.
    memcpy(ptr, ns.data(), nsLen);
    ptr[nsLen] = '\0';
    ptr += nsLenWithNull;

    // Write 'id'.
    memcpy(ptr, id, sizeof(ObjectIdType));

    return {TypeTags::bsonDBPointer, bitcastFrom<char*>(buffer.release())};
}

std::pair<TypeTags, Value> makeNewBsonCodeWScope(StringData code, const char* scope) {
    const auto codeLen = code.size();
    const auto codeLenWithNull = codeLen + sizeof(char);
    const auto scopeLen = ConstDataView(scope).read<LittleEndian<uint32_t>>();
    const auto numBytes = 2 * sizeof(uint32_t) + codeLenWithNull + scopeLen;
    auto buffer = std::make_unique<char[]>(numBytes);
    char* ptr = buffer.get();

    // Write length of 'numBytes' as a little-endian uint32_t.
    DataView(ptr).write<LittleEndian<uint32_t>>(numBytes);
    ptr += sizeof(uint32_t);

    // Write length of 'code' as a little-endian uint32_t.
    DataView(ptr).write<LittleEndian<uint32_t>>(codeLenWithNull);
    ptr += sizeof(uint32_t);

    // Write 'code' followed by a null terminator.
    memcpy(ptr, code.data(), codeLen);
    ptr[codeLen] = '\0';
    ptr += codeLenWithNull;

    // Write 'scope'.
    memcpy(ptr, scope, scopeLen);

    return {TypeTags::bsonCodeWScope, bitcastFrom<char*>(buffer.release())};
}

std::pair<TypeTags, Value> makeKeyString(const key_string::Value& inKey) {
    return {TypeTags::keyString,
            bitcastFrom<value::KeyStringEntry*>(new value::KeyStringEntry(inKey))};
}

std::pair<TypeTags, Value> makeCopyTimeZone(const TimeZone& tz) {
    auto tzCopy = std::make_unique<TimeZone>(tz);
    return {TypeTags::timeZone, bitcastFrom<TimeZone*>(tzCopy.release())};
}

std::pair<TypeTags, Value> makeCopyValueBlock(const ValueBlock& b) {
    auto cpy = b.clone();
    return {TypeTags::valueBlock, bitcastFrom<ValueBlock*>(cpy.release())};
}

std::pair<TypeTags, Value> makeCopyCellBlock(const CellBlock& b) {
    auto cpy = b.clone();
    return {TypeTags::cellBlock, bitcastFrom<CellBlock*>(cpy.release())};
}

std::pair<TypeTags, Value> makeCopyCollator(const CollatorInterface& collator) {
    auto collatorCopy = bitcastFrom<CollatorInterface*>(collator.clone().release());
    return {TypeTags::collator, collatorCopy};
}

std::pair<TypeTags, Value> makeNewRecordId(int64_t rid) {
    auto val = bitcastFrom<RecordId*>(new RecordId(rid));
    return {TypeTags::RecordId, val};
}

std::pair<TypeTags, Value> makeNewRecordId(const char* str, int32_t size) {
    auto val = bitcastFrom<RecordId*>(new RecordId(std::span(str, size)));
    return {TypeTags::RecordId, val};
}

std::pair<TypeTags, Value> makeCopyRecordId(const RecordId& rid) {
    auto copy = bitcastFrom<RecordId*>(new RecordId(rid));
    return {TypeTags::RecordId, copy};
}

std::pair<TypeTags, Value> makeCopyIndexBounds(const IndexBounds& bounds) {
    auto boundsCopy = bitcastFrom<IndexBounds*>(new IndexBounds(bounds));
    return {TypeTags::indexBounds, boundsCopy};
}

void releaseValueDeep(TypeTags tag, Value val) noexcept {
    switch (tag) {
        case TypeTags::RecordId:
            delete getRecordIdView(val);
            break;
        case TypeTags::NumberDecimal:
            delete[] getRawPointerView(val);
            break;
        case TypeTags::Array:
            delete getArrayView(val);
            break;
        case TypeTags::ArraySet:
            delete getArraySetView(val);
            break;
        case TypeTags::ArrayMultiSet:
            delete getArrayMultiSetView(val);
            break;
        case TypeTags::Object:
            delete getObjectView(val);
            break;
        case TypeTags::MultiMap:
            delete getMultiMapView(val);
            break;
        case TypeTags::ObjectId:
            delete getObjectIdView(val);
            break;
        case TypeTags::StringBig:
        case TypeTags::bsonSymbol:
        case TypeTags::bsonObjectId:
        case TypeTags::bsonBinData:
        case TypeTags::bsonRegex:
        case TypeTags::bsonJavascript:
        case TypeTags::bsonDBPointer:
        case TypeTags::bsonCodeWScope:
            delete[] getRawPointerView(val);
            break;
        case TypeTags::bsonArray:
        case TypeTags::bsonObject:
            UniqueBuffer::reclaim(getRawPointerView(val));
            break;
        case TypeTags::keyString:
            delete getKeyString(val);
            break;
        case TypeTags::collator:
            delete getCollatorView(val);
            break;
        case TypeTags::timeZone:
            delete getTimeZoneView(val);
            break;
        case TypeTags::valueBlock:
            delete getValueBlock(val);
            break;
        case TypeTags::cellBlock:
            delete getCellBlock(val);
            break;
        case TypeTags::pcreRegex:
        case TypeTags::jsFunction:
        case TypeTags::shardFilterer:
        case TypeTags::ftsMatcher:
        case TypeTags::sortSpec:
        case TypeTags::makeObjSpec:
        case TypeTags::indexBounds:
        case TypeTags::inList:
            getExtendedTypeOps(tag)->release(val);
            break;
        default:
            break;
    }
}

std::ostream& operator<<(std::ostream& os, const TypeTags tag) {
    ValuePrinters::make(os, PrintOptions()).writeTagToStream(tag);
    return os;
}

str::stream& operator<<(str::stream& str, const TypeTags tag) {
    ValuePrinters::make(str, PrintOptions()).writeTagToStream(tag);
    return str;
}

std::ostream& operator<<(std::ostream& os, const std::pair<TypeTags, Value>& value) {
    ValuePrinters::make(os, PrintOptions()).writeValueToStream(value.first, value.second);
    return os;
}

str::stream& operator<<(str::stream& str, const std::pair<TypeTags, Value>& value) {
    ValuePrinters::make(str, PrintOptions()).writeValueToStream(value.first, value.second);
    return str;
}

std::string print(const std::pair<TypeTags, Value>& value) {
    str::stream stream = str::stream();
    stream << value;
    return stream;
}

std::string printTagAndVal(const TypeTags tag, const Value value) {
    return printTagAndVal(std::pair<TypeTags, Value>{tag, value});
}

std::string printTagAndVal(const std::pair<TypeTags, Value>& value) {
    str::stream stream = str::stream();
    stream << "tag: " << value.first << ", val: " << value;
    return stream;
}

BSONType tagToType(TypeTags tag) noexcept {
    switch (tag) {
        case TypeTags::Nothing:
            return BSONType::eoo;
        case TypeTags::NumberInt32:
            return BSONType::numberInt;
        case TypeTags::NumberInt64:
            return BSONType::numberLong;
        case TypeTags::NumberDouble:
            return BSONType::numberDouble;
        case TypeTags::NumberDecimal:
            return BSONType::numberDecimal;
        case TypeTags::Date:
            return BSONType::date;
        case TypeTags::Timestamp:
            return BSONType::timestamp;
        case TypeTags::Boolean:
            return BSONType::boolean;
        case TypeTags::Null:
            return BSONType::null;
        case TypeTags::StringSmall:
            return BSONType::string;
        case TypeTags::StringBig:
            return BSONType::string;
        case TypeTags::Array:
            return BSONType::array;
        case TypeTags::ArraySet:
            return BSONType::array;
        case TypeTags::ArrayMultiSet:
            return BSONType::array;
        case TypeTags::Object:
            return BSONType::object;
        case TypeTags::ObjectId:
            return BSONType::oid;
        case TypeTags::MinKey:
            return BSONType::minKey;
        case TypeTags::MaxKey:
            return BSONType::maxKey;
        case TypeTags::bsonObject:
            return BSONType::object;
        case TypeTags::bsonArray:
            return BSONType::array;
        case TypeTags::bsonString:
            return BSONType::string;
        case TypeTags::bsonSymbol:
            return BSONType::symbol;
        case TypeTags::bsonObjectId:
            return BSONType::oid;
        case TypeTags::bsonBinData:
            return BSONType::binData;
        case TypeTags::bsonUndefined:
            return BSONType::undefined;
        case TypeTags::bsonRegex:
            return BSONType::regEx;
        case TypeTags::bsonJavascript:
            return BSONType::code;
        case TypeTags::bsonDBPointer:
            return BSONType::dbRef;
        case TypeTags::bsonCodeWScope:
            return BSONType::codeWScope;
        default:
            return BSONType::eoo;
    }
}

inline std::size_t hashObjectId(const uint8_t* objId) noexcept {
    auto dataView = ConstDataView(reinterpret_cast<const char*>(objId));
    return abslHash(dataView.read<LittleEndian<uint64_t>>()) ^
        abslHash(dataView.read<LittleEndian<uint32_t>>(sizeof(uint64_t)));
}

std::size_t hashValue(TypeTags tag, Value val, const CollatorInterface* collator) noexcept {
    switch (tag) {
        case TypeTags::NumberInt32:
            return abslHash(static_cast<int64_t>(bitcastTo<int32_t>(val)));
        case TypeTags::RecordId:
            return getRecordIdView(val)->hash();
        case TypeTags::NumberInt64:
            return abslHash(bitcastTo<int64_t>(val));
        case TypeTags::NumberDouble: {
            // Force doubles to integers for hashing.
            auto dbl = bitcastTo<double>(val);
            if (auto asInt = representAs<int64_t>(dbl); asInt) {
                return abslHash(*asInt);
            } else if (std::isnan(dbl)) {
                return abslHash(std::numeric_limits<double>::quiet_NaN());
            } else {
                // Doubles not representable as int64_t will hash as doubles.
                return abslHash(dbl);
            }
        }
        case TypeTags::NumberDecimal: {
            // Force decimals to integers for hashing.
            auto dec = bitcastTo<Decimal128>(val);
            if (auto asInt = representAs<int64_t>(dec); asInt) {
                return abslHash(*asInt);
            } else if (dec.isNaN()) {
                return abslHash(std::numeric_limits<double>::quiet_NaN());
            } else if (auto asDbl = representAs<double>(dec); asDbl) {
                return abslHash(*asDbl);
            } else {
                return abslHash(dec.getValue().low64) ^ abslHash(dec.getValue().high64);
            }
        }
        case TypeTags::Date:
            return abslHash(bitcastTo<int64_t>(val));
        case TypeTags::Timestamp:
            return abslHash(bitcastTo<uint64_t>(val));
        case TypeTags::Boolean:
            return bitcastTo<bool>(val);
        case TypeTags::Null:
        case TypeTags::MinKey:
        case TypeTags::MaxKey:
        case TypeTags::bsonUndefined:
            return 0;
        case TypeTags::StringSmall:
        case TypeTags::StringBig:
        case TypeTags::bsonString:
        case TypeTags::bsonSymbol: {
            auto sv = getStringOrSymbolView(tag, val);
            if (collator) {
                return abslHash(collator->getComparisonKey(sv).getKeyData());
            } else {
                return abslHash(sv);
            }
        }
        case TypeTags::ObjectId:
        case TypeTags::bsonObjectId: {
            auto objId =
                tag == TypeTags::ObjectId ? getObjectIdView(val)->data() : bitcastTo<uint8_t*>(val);
            return hashObjectId(objId);
        }
        case TypeTags::keyString:
            return getKeyString(val)->hash();
        case TypeTags::Array:
        case TypeTags::bsonArray: {
            auto arr = ArrayEnumerator{tag, val};
            auto res = hashInit();

            // There should be enough entropy in the first 4 elements.
            for (int i = 0; i < 4 && !arr.atEnd(); ++i) {
                auto [elemTag, elemVal] = arr.getViewOfValue();
                res = hashCombine(res, hashValue(elemTag, elemVal, collator));
                arr.advance();
            }

            return res;
        }
        case TypeTags::ArraySet:
        case TypeTags::ArrayMultiSet: {
            size_t size = tag == TypeTags::ArraySet ? getArraySetView(val)->size()
                                                    : getArrayMultiSetView(val)->size();
            std::vector<size_t> valueHashes;
            valueHashes.reserve(size);
            for (ArrayEnumerator arr{tag, val}; !arr.atEnd(); arr.advance()) {
                auto [elemTag, elemVal] = arr.getViewOfValue();
                valueHashes.push_back(hashValue(elemTag, elemVal));
            }
            // TODO SERVER-92666 Implement a more efficient hashing algorithm
            std::sort(valueHashes.begin(), valueHashes.end());
            return std::accumulate(
                valueHashes.begin(), valueHashes.end(), hashInit(), &hashCombine);
        }
        case TypeTags::Object:
        case TypeTags::bsonObject: {
            auto obj = ObjectEnumerator{tag, val};
            auto res = hashInit();

            // There should be enough entropy in the first 4 elements.
            for (int i = 0; i < 4 && !obj.atEnd(); ++i) {
                auto [elemTag, elemVal] = obj.getViewOfValue();
                res = hashCombine(res, hashValue(elemTag, elemVal, collator));
                obj.advance();
            }

            return res;
        }
        case TypeTags::MultiMap: {
            auto multiMap = getMultiMapView(val);
            auto res = hashInit();

            for (const auto& [key, value] : multiMap->values()) {
                res = hashCombine(res, hashValue(key.first, key.second, collator));
                res = hashCombine(res, hashValue(value.first, value.second, collator));
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
                return abslHash(ConstDataView(buffer).read<LittleEndian<uint64_t>>());
            } else {
                // Hash only the first 8 bytes. It should be enough.
                auto dataView = ConstDataView(getRawPointerView(val) + sizeof(uint32_t));
                return abslHash(dataView.read<LittleEndian<uint64_t>>());
            }
        }
        case TypeTags::bsonRegex: {
            auto regex = getBsonRegexView(val);
            return hashCombine(hashCombine(hashInit(), abslHash(regex.pattern)),
                               abslHash(regex.flags));
        }
        case TypeTags::bsonJavascript:
            return abslHash(getBsonJavascriptView(val));
        case TypeTags::bsonDBPointer: {
            auto dbptr = getBsonDBPointerView(val);
            return hashCombine(hashCombine(hashInit(), abslHash(dbptr.ns)), hashObjectId(dbptr.id));
        }
        case TypeTags::bsonCodeWScope: {
            auto cws = getBsonCodeWScopeView(val);

            // Collation semantics do not apply to strings nested inside the CodeWScope scope
            // object, so we do not pass through the collator when computing the hash of the
            // scope object.
            return hashCombine(
                hashCombine(hashInit(), abslHash(cws.code)),
                hashValue(TypeTags::bsonObject, bitcastFrom<const char*>(cws.scope)));
        }
        case TypeTags::timeZone: {
            auto timezone = getTimeZoneView(val);
            return hashCombine(hashCombine(hashInit(), abslHash(timezone->getTzInfo())),
                               abslHash(timezone->getUtcOffset().count()));
        }
        default:
            break;
    }

    return 0;
}

/*
 * Three ways value comparison (aka spacehip operator).
 */
std::pair<TypeTags, Value> compareValue(TypeTags lhsTag,
                                        Value lhsValue,
                                        TypeTags rhsTag,
                                        Value rhsValue,
                                        const StringDataComparator* comparator) {
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
                auto result = [&]() {
                    if (lhsTag == TypeTags::NumberInt64) {
                        return compareLongToDouble(bitcastTo<int64_t>(lhsValue),
                                                   bitcastTo<double>(rhsValue));
                    } else if (rhsTag == TypeTags::NumberInt64) {
                        return compareDoubleToLong(bitcastTo<double>(lhsValue),
                                                   bitcastTo<int64_t>(rhsValue));
                    } else {
                        return compareDoubles(numericCast<double>(lhsTag, lhsValue),
                                              numericCast<double>(rhsTag, rhsValue));
                    }
                }();
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
            }
            case TypeTags::NumberDecimal: {
                auto result = [&]() {
                    if (lhsTag == TypeTags::NumberDouble) {
                        return compareDoubleToDecimal(numericCast<double>(lhsTag, lhsValue),
                                                      numericCast<Decimal128>(rhsTag, rhsValue));
                    } else if (rhsTag == TypeTags::NumberDouble) {
                        return compareDecimalToDouble(numericCast<Decimal128>(lhsTag, lhsValue),
                                                      numericCast<double>(rhsTag, rhsValue));
                    } else {
                        return compareDecimals(numericCast<Decimal128>(lhsTag, lhsValue),
                                               numericCast<Decimal128>(rhsTag, rhsValue));
                    }
                }();
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (isStringOrSymbol(lhsTag) && isStringOrSymbol(rhsTag)) {
        auto lhsStr = getStringOrSymbolView(lhsTag, lhsValue);
        auto rhsStr = getStringOrSymbolView(rhsTag, rhsValue);

        auto result = comparator ? comparator->compare(lhsStr, rhsStr) : lhsStr.compare(rhsStr);

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
    } else if (lhsTag == TypeTags::MinKey && rhsTag == TypeTags::MinKey) {
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else if (lhsTag == TypeTags::MaxKey && rhsTag == TypeTags::MaxKey) {
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else if (lhsTag == TypeTags::bsonUndefined && rhsTag == TypeTags::bsonUndefined) {
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else if (isArray(lhsTag) && isArray(rhsTag)) {
        // ArraySets and ArrayMultiSet carry semantics of an unordered set, so we cannot define a
        // deterministic less or greater operations on them, but only compare for equality.
        // Comparing an ArraySet or ArrayMultiSet with a regular Array is equivalent of converting
        // the ArraySet/ArrayMultiSet to an Array and them comparing the two Arrays, so we can
        // simply use a generic algorithm below.
        if (lhsTag == TypeTags::ArraySet && rhsTag == TypeTags::ArraySet) {
            auto lhsArr = getArraySetView(lhsValue);
            auto rhsArr = getArraySetView(rhsValue);
            if (*lhsArr == *rhsArr) {
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
            }
            return {TypeTags::Nothing, 0};
        }

        if (lhsTag == TypeTags::ArrayMultiSet && rhsTag == TypeTags::ArrayMultiSet) {
            auto lhsArr = getArrayMultiSetView(lhsValue);
            auto rhsArr = getArrayMultiSetView(rhsValue);
            if (*lhsArr == *rhsArr) {
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
            }
            // If they are not equal then we cannot say if one is smaller than the other.
            return {TypeTags::Nothing, 0};
        }

        auto lhsArr = ArrayEnumerator{lhsTag, lhsValue};
        auto rhsArr = ArrayEnumerator{rhsTag, rhsValue};
        while (!lhsArr.atEnd() && !rhsArr.atEnd()) {
            auto [lhsTag, lhsVal] = lhsArr.getViewOfValue();
            auto [rhsTag, rhsVal] = rhsArr.getViewOfValue();

            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
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
    } else if (lhsTag == TypeTags::MultiMap && rhsTag == TypeTags::MultiMap) {
        auto lhsMap = getMultiMapView(lhsValue);
        auto rhsMap = getMultiMapView(rhsValue);
        if (*lhsMap == *rhsMap) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
        }
        // If they are not equal then we cannot say if one is smaller than the other.
        return {TypeTags::Nothing, 0};
    } else if (isObject(lhsTag) && isObject(rhsTag)) {
        auto lhsObj = ObjectEnumerator{lhsTag, lhsValue};
        auto rhsObj = ObjectEnumerator{rhsTag, rhsValue};
        while (!lhsObj.atEnd() && !rhsObj.atEnd()) {
            // To match BSONElement::woCompare() semantics, we first compare the canonical types of
            // the elements. If they do not match, we return their difference.
            auto [lhsTag, lhsVal] = lhsObj.getViewOfValue();
            auto [rhsTag, rhsVal] = rhsObj.getViewOfValue();


            if (auto result = canonicalizeBSONType(tagToType(lhsTag)) -
                    canonicalizeBSONType(tagToType(rhsTag));
                result != 0) {
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
            }

            auto fieldCmp = lhsObj.getFieldName().compare(rhsObj.getFieldName());
            if (fieldCmp != 0) {
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(fieldCmp, 0))};
            }

            auto [tag, val] = compareValue(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
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
    } else if (lhsTag == TypeTags::keyString && rhsTag == TypeTags::keyString) {
        auto result = getKeyString(lhsValue)->compare(*getKeyString(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
    } else if (lhsTag == TypeTags::Nothing && rhsTag == TypeTags::Nothing) {
        // Special case for Nothing in a hash table (group) and sort comparison.
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else if (lhsTag == TypeTags::RecordId && rhsTag == TypeTags::RecordId) {
        int32_t result = getRecordIdView(lhsValue)->compare(*getRecordIdView(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    } else if (lhsTag == TypeTags::bsonRegex && rhsTag == TypeTags::bsonRegex) {
        auto lhsRegex = getBsonRegexView(lhsValue);
        auto rhsRegex = getBsonRegexView(rhsValue);
        if (auto result = lhsRegex.pattern.compare(rhsRegex.pattern); result != 0) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
        }

        auto result = lhsRegex.flags.compare(rhsRegex.flags);
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    } else if (lhsTag == TypeTags::bsonJavascript && rhsTag == TypeTags::bsonJavascript) {
        auto lhsCode = getBsonJavascriptView(lhsValue);
        auto rhsCode = getBsonJavascriptView(rhsValue);
        auto result = compareHelper(lhsCode, rhsCode);
        return {TypeTags::NumberInt32, result};
    } else if (lhsTag == TypeTags::bsonDBPointer && rhsTag == TypeTags::bsonDBPointer) {
        // To match the existing behavior from the classic execution engine, we intentionally
        // compare the sizes of 'ns' fields first, and then only if the sizes are equal do we
        // compare the contents of the 'ns' fields.
        auto lhsDBPtr = getBsonDBPointerView(lhsValue);
        auto rhsDBPtr = getBsonDBPointerView(rhsValue);
        if (lhsDBPtr.ns.size() != rhsDBPtr.ns.size()) {
            return {TypeTags::NumberInt32,
                    bitcastFrom<int32_t>(compareHelper(lhsDBPtr.ns.size(), rhsDBPtr.ns.size()))};
        }

        if (auto result = lhsDBPtr.ns.compare(rhsDBPtr.ns); result != 0) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
        }

        auto result = memcmp(lhsDBPtr.id, rhsDBPtr.id, sizeof(ObjectIdType));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    } else if (lhsTag == TypeTags::bsonCodeWScope && rhsTag == TypeTags::bsonCodeWScope) {
        auto lhsCws = getBsonCodeWScopeView(lhsValue);
        auto rhsCws = getBsonCodeWScopeView(rhsValue);
        if (auto result = lhsCws.code.compare(rhsCws.code); result != 0) {
            return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
        }

        // Special string comparison semantics do not apply to strings nested inside the
        // CodeWScope scope object, so we do not pass through the string comparator.
        return compareValue(TypeTags::bsonObject,
                            bitcastFrom<const char*>(lhsCws.scope),
                            TypeTags::bsonObject,
                            bitcastFrom<const char*>(rhsCws.scope));
    } else {
        // Different types.
        if (lhsTag == TypeTags::Nothing || rhsTag == TypeTags::Nothing) {
            return {TypeTags::Nothing, 0};
        }
        auto lhsType = tagToType(lhsTag);
        auto rhsType = tagToType(rhsTag);
        tassert(5365500, "values cannot have the same type", lhsType != rhsType);
        auto result = canonicalizeBSONType(lhsType) - canonicalizeBSONType(rhsType);
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(compareHelper(result, 0))};
    }
}  // compareValue

bool isNaN(TypeTags tag, Value val) noexcept {
    return (tag == TypeTags::NumberDouble && std::isnan(bitcastTo<double>(val))) ||
        (tag == TypeTags::NumberDecimal && bitcastTo<Decimal128>(val).isNaN());
}

bool isInfinity(TypeTags tag, Value val) noexcept {
    return (tag == TypeTags::NumberDouble && std::isinf(bitcastTo<double>(val))) ||
        (tag == TypeTags::NumberDecimal && bitcastTo<Decimal128>(val).isInfinite());
}

bool ArraySet::push_back(TypeTags tag, Value val) {
    if (tag != TypeTags::Nothing) {
        ValueGuard guard{tag, val};
        auto [it, inserted] = _values.insert({tag, val});

        if (inserted) {
            guard.reset();
        }

        return inserted;
    }

    return false;
}

bool ArraySet::push_back_clone(TypeTags tag, Value val) {
    if (tag != TypeTags::Nothing) {
        return _values.insert_lazy({tag, val}, [&]() { return value::copyValue(tag, val); }).second;
    }

    return false;
}

std::pair<TypeTags, Value> makeNewArraySet(TypeTags tag,
                                           Value value,
                                           const CollatorInterface* collator) {
    auto [resTag, resVal] = makeNewArraySet(collator);
    ValueGuard guard(resTag, resVal);
    ArraySet* setValues = getArraySetView(resVal);
    setValues->reserve(getArraySize(tag, value));
    arrayForEach(tag, value, [&](TypeTags elemTag, Value elemVal) {
        setValues->push_back_clone(elemTag, elemVal);
    });
    guard.reset();
    return {resTag, reinterpret_cast<Value>(setValues)};
}

std::pair<TypeTags, Value> ArrayEnumerator::getViewOfValue() const {
    if (_array) {
        return _array->getAt(_index);
    } else if (_arraySet) {
        return {_arraySetIter->first, _arraySetIter->second};
    } else if (_arrayMultiSet) {
        return {_arrayMultiSetIter->first, _arrayMultiSetIter->second};
    } else {
        return bson::convertFrom<true>(_arrayCurrent, _arrayEnd, _fieldNameSize);
    }
}

bool ArrayEnumerator::advance() {
    if (_array) {
        if (_index < _array->size()) {
            ++_index;
        }

        return _index < _array->size();
    } else if (_arraySet) {
        if (_arraySetIter != _arraySet->values().end()) {
            ++_arraySetIter;
        }

        return _arraySetIter != _arraySet->values().end();
    } else if (_arrayMultiSet) {
        if (_arrayMultiSetIter != _arrayMultiSet->values().end()) {
            ++_arrayMultiSetIter;
        }

        return _arrayMultiSetIter != _arrayMultiSet->values().end();
    } else {
        if (_arrayCurrent != _arrayEnd - 1) {
            _arrayCurrent = bson::advance(_arrayCurrent, _fieldNameSize);
            if (_arrayCurrent != _arrayEnd - 1) {
                _fieldNameSize = TinyStrHelpers::strlen(bson::fieldNameRaw(_arrayCurrent));
            }
        }

        return _arrayCurrent != _arrayEnd - 1;
    }
}

std::pair<TypeTags, Value> ObjectEnumerator::getViewOfValue() const {
    if (_object) {
        return _object->getAt(_index);
    } else {
        auto sv = bson::fieldNameAndLength(_objectCurrent);
        return bson::convertFrom<true>(_objectCurrent, _objectEnd, sv.size());
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
            auto sv = bson::fieldNameAndLength(_objectCurrent);
            _objectCurrent = bson::advance(_objectCurrent, sv.size());
        }

        return *_objectCurrent != 0;
    }
}

StringData ObjectEnumerator::getFieldName() const {
    using namespace std::literals;
    if (_object) {
        if (_index < _object->size()) {
            return _object->field(_index);
        } else {
            return ""_sd;
        }
    } else {
        if (*_objectCurrent != 0) {
            return bson::fieldNameAndLength(_objectCurrent);
        } else {
            return ""_sd;
        }
    }
}

void readKeyStringValueIntoAccessors(const SortedDataKeyValueView& keyString,
                                     const Ordering& ordering,
                                     BufBuilder* valueBufferBuilder,
                                     std::vector<OwnedValueAccessor>* accessors,
                                     boost::optional<IndexKeysInclusionSet> indexKeysToInclude) {
    OwnedValueAccessorValueBuilder valBuilder(valueBufferBuilder);
    invariant(!indexKeysToInclude || indexKeysToInclude->count() == accessors->size());

    auto ks = keyString.getKeyStringWithoutRecordIdView();
    BufReader reader(ks.data(), ks.size());

    auto typeBits = keyString.getTypeBitsView();
    BufReader typeBitsBr(typeBits.data(), typeBits.size());
    auto typeBitsReader =
        key_string::TypeBits::getReaderFromBuffer(keyString.getVersion(), &typeBitsBr);

    bool keepReading = true;
    size_t componentIndex = 0;
    do {
        // In the edge case that 'componentIndex' indicates that we have already read
        // 'kMaxCompoundIndexKeys' components, we expect that the next 'readValue()' will
        // return false (to indicate EOF), so the value of 'inverted' does not matter.
        bool inverted = (componentIndex < Ordering::kMaxCompoundIndexKeys)
            ? (ordering.get(componentIndex) == -1)
            : false;

        keepReading = key_string::readValue(
            &reader, &typeBitsReader, inverted, keyString.getVersion(), &valBuilder);

        invariant(componentIndex < Ordering::kMaxCompoundIndexKeys || !keepReading);

        // If 'indexKeysToInclude' indicates that this index key component is not part of the
        // projection, remove it from the list of values that will be fed to the 'accessors'
        // list. Note that, even when we are excluding a key component, we can't skip the call
        // to 'key_string::readValue()' because it is needed to advance the 'reader' and
        // 'typeBitsReader' stream.
        if (indexKeysToInclude && (componentIndex < Ordering::kMaxCompoundIndexKeys) &&
            !(*indexKeysToInclude)[componentIndex]) {
            valBuilder.popValue();
        }
        ++componentIndex;
    } while (keepReading && valBuilder.numValues() < accessors->size());

    valBuilder.readValues(accessors);
}

std::pair<TypeTags, Value> arrayToSet(TypeTags tag, Value val, CollatorInterface* collator) {
    if (!isArray(tag)) {
        return {TypeTags::Nothing, 0};
    }

    if (tag == TypeTags::ArraySet) {
        auto arrSet = getArraySetView(val);

        if (CollatorInterface::collatorsMatch(collator, arrSet->getCollator())) {
            return makeCopyArraySet(*arrSet);
        }
    }

    auto [setTag, setVal] = makeNewArraySet(collator);
    ValueGuard guard{setTag, setVal};
    auto setView = getArraySetView(setVal);

    auto arrIter = ArrayEnumerator{tag, val};
    while (!arrIter.atEnd()) {
        auto [elTag, elVal] = arrIter.getViewOfValue();
        auto [copyTag, copyVal] = copyValue(elTag, elVal);
        setView->push_back(copyTag, copyVal);
        arrIter.advance();
    }
    guard.reset();
    return {setTag, setVal};
}

bool operator==(const ArraySet& lhs, const ArraySet& rhs) {
    return lhs.values() == rhs.values();
}

bool operator!=(const ArraySet& lhs, const ArraySet& rhs) {
    return !(lhs == rhs);
}

bool operator==(const MultiMap& lhs, const MultiMap& rhs) {
    return lhs.values() == rhs.values();
}

bool operator!=(const MultiMap& lhs, const MultiMap& rhs) {
    return !(lhs == rhs);
}

std::pair<TypeTags, Value> genericEq(TypeTags lhsTag,
                                     Value lhsVal,
                                     TypeTags rhsTag,
                                     Value rhsVal,
                                     const StringDataComparator* comparator) {
    return genericCompare<std::equal_to<>>(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
}

std::pair<TypeTags, Value> genericNeq(TypeTags lhsTag,
                                      Value lhsVal,
                                      TypeTags rhsTag,
                                      Value rhsVal,
                                      const StringDataComparator* comparator) {
    auto [tag, val] = genericEq(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
    // genericEq() will return either Boolean or Nothing. If it returns Boolean, negate
    // 'val' before returning it.
    val = tag == TypeTags::Boolean ? bitcastFrom<bool>(!bitcastTo<bool>(val)) : val;
    return {tag, val};
}

std::pair<TypeTags, Value> genericLt(TypeTags lhsTag,
                                     Value lhsVal,
                                     TypeTags rhsTag,
                                     Value rhsVal,
                                     const StringDataComparator* comparator) {
    return genericCompare<std::less<>>(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
}

std::pair<TypeTags, Value> genericLte(TypeTags lhsTag,
                                      Value lhsVal,
                                      TypeTags rhsTag,
                                      Value rhsVal,
                                      const StringDataComparator* comparator) {
    return genericCompare<std::less_equal<>>(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
}

std::pair<TypeTags, Value> genericGt(TypeTags lhsTag,
                                     Value lhsVal,
                                     TypeTags rhsTag,
                                     Value rhsVal,
                                     const StringDataComparator* comparator) {
    return genericCompare<std::greater<>>(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
}

std::pair<TypeTags, Value> genericGte(TypeTags lhsTag,
                                      Value lhsVal,
                                      TypeTags rhsTag,
                                      Value rhsVal,
                                      const StringDataComparator* comparator) {
    return genericCompare<std::greater_equal<>>(lhsTag, lhsVal, rhsTag, rhsVal, comparator);
}
}  // namespace value
}  // namespace sbe
}  // namespace mongo
