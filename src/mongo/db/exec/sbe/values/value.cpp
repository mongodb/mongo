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

#include "mongo/base/compare_numbers.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre_util.h"

namespace mongo {
namespace sbe {
namespace value {

namespace {
template <typename T>
auto abslHash(const T& val) {
    if constexpr (std::is_same_v<T, StringData>) {
        return absl::Hash<absl::string_view>{}(absl::string_view{val.rawData(), val.size()});
    } else if constexpr (IsEndian<T>::value) {
        return abslHash(val.value);
    } else {
        return absl::Hash<T>{}(val);
    }
}
}  // namespace

std::pair<TypeTags, Value> makeNewBsonRegex(StringData pattern, StringData flags) {
    // Add 2 to account NULL bytes after pattern and flags.
    auto totalSize = pattern.size() + flags.size() + 2;
    auto buffer = std::make_unique<char[]>(totalSize);
    auto rawBuffer = buffer.get();

    // Copy pattern first and flags after it.
    memcpy(rawBuffer, pattern.rawData(), pattern.size());
    memcpy(rawBuffer + pattern.size() + 1, flags.rawData(), flags.size());

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
    memcpy(ptr, ns.rawData(), nsLen);
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
    memcpy(ptr, code.rawData(), codeLen);
    ptr[codeLen] = '\0';
    ptr += codeLenWithNull;

    // Write 'scope'.
    memcpy(ptr, scope, scopeLen);

    return {TypeTags::bsonCodeWScope, bitcastFrom<char*>(buffer.release())};
}

std::pair<TypeTags, Value> makeCopyKeyString(const KeyString::Value& inKey) {
    auto k = new KeyString::Value(inKey);
    return {TypeTags::ksValue, bitcastFrom<KeyString::Value*>(k)};
}

std::pair<TypeTags, Value> makeNewPcreRegex(StringData pattern, StringData options) {
    auto regex =
        std::make_unique<pcre::Regex>(std::string{pattern}, pcre_util::flagsToOptions(options));
    uassert(5073402, str::stream() << "Invalid Regex: " << errorMessage(regex->error()), *regex);
    return {TypeTags::pcreRegex, bitcastFrom<pcre::Regex*>(regex.release())};
}

std::pair<TypeTags, Value> makeCopyPcreRegex(const pcre::Regex& regex) {
    auto regexCopy = std::make_unique<pcre::Regex>(regex);
    return {TypeTags::pcreRegex, bitcastFrom<pcre::Regex*>(regexCopy.release())};
}

KeyString::Value SortSpec::generateSortKey(const BSONObj& obj, const CollatorInterface* collator) {
    KeyStringSet keySet;
    SharedBufferFragmentBuilder allocator(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
    const bool skipMultikey = false;
    MultikeyPaths* multikeyPaths = nullptr;
    _keyGen.getKeys(allocator, obj, skipMultikey, &keySet, multikeyPaths, collator);

    // When 'isSparse' is false, BtreeKeyGenerator::getKeys() is guaranteed to insert at least
    // one key into 'keySet', so this assertion should always be true.
    tassert(5037000, "BtreeKeyGenerator failed to generate key", !keySet.empty());

    // Return the first KeyString in the set.
    return std::move(*keySet.extract_sequence().begin());
}

BtreeKeyGenerator SortSpec::initKeyGen() const {
    tassert(
        5037003, "SortSpec should not be passed an empty sort pattern", !_sortPattern.isEmpty());

    std::vector<const char*> fields;
    std::vector<BSONElement> fixed;
    for (auto&& elem : _sortPattern) {
        fields.push_back(elem.fieldName());

        // BtreeKeyGenerator's constructor's first parameter (the 'fields' vector) and second
        // parameter (the 'fixed' vector) are parallel vectors. The 'fixed' vector allows the
        // caller to specify if the any sort keys have already been determined for one or more
        // of the field paths from the 'fields' vector. In this case, we haven't determined what
        // the sort keys are for any of the fields paths, so we populate the 'fixed' vector with
        // EOO values to indicate this.
        fixed.emplace_back();
    }

    const bool isSparse = false;
    auto version = KeyString::Version::kLatestVersion;
    auto ordering = Ordering::make(_sortPattern);

    return {std::move(fields), std::move(fixed), isSparse, version, ordering};
}

size_t SortSpec::getApproximateSize() const {
    auto size = sizeof(SortSpec);
    size += _sortPattern.isOwned() ? _sortPattern.objsize() : 0;
    size += _keyGen.getApproximateSize() - sizeof(_keyGen);
    return size;
}

std::pair<TypeTags, Value> makeCopyJsFunction(const JsFunction& jsFunction) {
    auto ownedJsFunction = bitcastFrom<JsFunction*>(new JsFunction(jsFunction));
    return {TypeTags::jsFunction, ownedJsFunction};
}

std::pair<TypeTags, Value> makeCopyShardFilterer(const ShardFilterer& filterer) {
    auto filtererCopy = bitcastFrom<ShardFilterer*>(filterer.clone().release());
    return {TypeTags::shardFilterer, filtererCopy};
}

std::pair<TypeTags, Value> makeCopyFtsMatcher(const fts::FTSMatcher& matcher) {
    auto copy = bitcastFrom<fts::FTSMatcher*>(new fts::FTSMatcher(matcher.query(), matcher.spec()));
    return {TypeTags::ftsMatcher, copy};
}

std::pair<TypeTags, Value> makeCopySortSpec(const SortSpec& ss) {
    auto ssCopy = bitcastFrom<SortSpec*>(new SortSpec(ss));
    return {TypeTags::sortSpec, ssCopy};
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
    auto val = bitcastFrom<RecordId*>(new RecordId(str, size));
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


void releaseValue(TypeTags tag, Value val) noexcept {
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
        case TypeTags::Object:
            delete getObjectView(val);
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
        case TypeTags::ksValue:
            delete getKeyStringView(val);
            break;
        case TypeTags::pcreRegex:
            delete getPcreRegexView(val);
            break;
        case TypeTags::jsFunction:
            delete getJsFunctionView(val);
            break;
        case TypeTags::shardFilterer:
            delete getShardFiltererView(val);
            break;
        case TypeTags::ftsMatcher:
            delete getFtsMatcherView(val);
            break;
        case TypeTags::sortSpec:
            delete getSortSpecView(val);
            break;
        case TypeTags::collator:
            delete getCollatorView(val);
            break;
        case TypeTags::indexBounds:
            delete getIndexBoundsView(val);
            break;
        case TypeTags::classicMatchExpresion:
            delete getClassicMatchExpressionView(val);
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

BSONType tagToType(TypeTags tag) noexcept {
    switch (tag) {
        case TypeTags::Nothing:
            return BSONType::EOO;
        case TypeTags::NumberInt32:
            return BSONType::NumberInt;
        case TypeTags::RecordId:
            return BSONType::EOO;
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
        case TypeTags::MinKey:
            return BSONType::MinKey;
        case TypeTags::MaxKey:
            return BSONType::MaxKey;
        case TypeTags::bsonObject:
            return BSONType::Object;
        case TypeTags::bsonArray:
            return BSONType::Array;
        case TypeTags::bsonString:
            return BSONType::String;
        case TypeTags::bsonSymbol:
            return BSONType::Symbol;
        case TypeTags::bsonObjectId:
            return BSONType::jstOID;
        case TypeTags::bsonBinData:
            return BSONType::BinData;
        case TypeTags::bsonUndefined:
            return BSONType::Undefined;
        case TypeTags::ksValue:
            // This is completely arbitrary.
            return BSONType::EOO;
        case TypeTags::bsonRegex:
            return BSONType::RegEx;
        case TypeTags::bsonJavascript:
            return BSONType::Code;
        case TypeTags::bsonDBPointer:
            return BSONType::DBRef;
        case TypeTags::bsonCodeWScope:
            return BSONType::CodeWScope;
        default:
            MONGO_UNREACHABLE;
    }
}

bool isShallowType(TypeTags tag) noexcept {
    switch (tag) {
        case TypeTags::Nothing:
        case TypeTags::Null:
        case TypeTags::NumberInt32:
        case TypeTags::NumberInt64:
        case TypeTags::NumberDouble:
        case TypeTags::Date:
        case TypeTags::Timestamp:
        case TypeTags::Boolean:
        case TypeTags::StringSmall:
        case TypeTags::MinKey:
        case TypeTags::MaxKey:
        case TypeTags::bsonUndefined:
        case TypeTags::LocalLambda:
            return true;
        case TypeTags::RecordId:
        case TypeTags::NumberDecimal:
        case TypeTags::StringBig:
        case TypeTags::bsonString:
        case TypeTags::bsonSymbol:
        case TypeTags::Array:
        case TypeTags::ArraySet:
        case TypeTags::Object:
        case TypeTags::ObjectId:
        case TypeTags::bsonObjectId:
        case TypeTags::bsonObject:
        case TypeTags::bsonArray:
        case TypeTags::bsonBinData:
        case TypeTags::ksValue:
        case TypeTags::bsonRegex:
        case TypeTags::bsonJavascript:
        case TypeTags::bsonDBPointer:
            return false;
        default:
            MONGO_UNREACHABLE;
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
            return abslHash(bitcastTo<int32_t>(val));
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
        case TypeTags::ksValue:
            return getKeyStringView(val)->hash();
        case TypeTags::Array:
        case TypeTags::ArraySet:
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
 * The StringData::compare(basic_string_view s) function, for example, only promises that it
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
                                        Value rhsValue,
                                        const StringData::ComparatorInterface* comparator) {
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
        // ArraySets carry semantics of an unordered set, so we cannot define a deterministic
        // less or greater operations on them, but only compare for equality. Comparing an
        // ArraySet with a regular Array is equivalent of converting the ArraySet to an Array
        // and them comparing the two Arrays, so we can simply use a generic algorithm below.
        if (lhsTag == TypeTags::ArraySet && rhsTag == TypeTags::ArraySet) {
            auto lhsArr = getArraySetView(lhsValue);
            auto rhsArr = getArraySetView(rhsValue);
            if (*lhsArr == *rhsArr) {
                return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
            }
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
    } else if (lhsTag == TypeTags::ksValue && rhsTag == TypeTags::ksValue) {
        auto result = getKeyStringView(lhsValue)->compare(*getKeyStringView(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
    } else if (lhsTag == TypeTags::Nothing && rhsTag == TypeTags::Nothing) {
        // Special case for Nothing in a hash table (group) and sort comparison.
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(0)};
    } else if (lhsTag == TypeTags::RecordId && rhsTag == TypeTags::RecordId) {
        int32_t result = getRecordIdView(lhsValue)->compare(*getRecordIdView(rhsValue));
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(result)};
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
}

bool isNaN(TypeTags tag, Value val) {
    return (tag == TypeTags::NumberDouble && std::isnan(bitcastTo<double>(val))) ||
        (tag == TypeTags::NumberDecimal && bitcastTo<Decimal128>(val).isNaN());
}

bool isInfinity(TypeTags tag, Value val) {
    return (tag == TypeTags::NumberDouble && std::isinf(bitcastTo<double>(val))) ||
        (tag == TypeTags::NumberDecimal && bitcastTo<Decimal128>(val).isInfinite());
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
        return bson::convertFrom<true>(_arrayCurrent, _arrayEnd, sv.size());
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
            auto sv = bson::fieldNameView(_objectCurrent);
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
            return bson::fieldNameView(_objectCurrent);
        } else {
            return ""_sd;
        }
    }
}

void readKeyStringValueIntoAccessors(const KeyString::Value& keyString,
                                     const Ordering& ordering,
                                     BufBuilder* valueBufferBuilder,
                                     std::vector<OwnedValueAccessor>* accessors,
                                     boost::optional<IndexKeysInclusionSet> indexKeysToInclude) {
    OwnedValueAccessorValueBuilder valBuilder(valueBufferBuilder);
    invariant(!indexKeysToInclude || indexKeysToInclude->count() == accessors->size());

    BufReader reader(keyString.getBuffer(), keyString.getSize());
    KeyString::TypeBits typeBits(keyString.getTypeBits());
    KeyString::TypeBits::Reader typeBitsReader(typeBits);

    bool keepReading = true;
    size_t componentIndex = 0;
    do {
        // In the edge case that 'componentIndex' indicates that we have already read
        // 'kMaxCompoundIndexKeys' components, we expect that the next 'readSBEValue()' will
        // return false (to indicate EOF), so the value of 'inverted' does not matter.
        bool inverted = (componentIndex < Ordering::kMaxCompoundIndexKeys)
            ? (ordering.get(componentIndex) == -1)
            : false;

        keepReading = KeyString::readSBEValue(
            &reader, &typeBitsReader, inverted, typeBits.version, &valBuilder);

        invariant(componentIndex < Ordering::kMaxCompoundIndexKeys || !keepReading);

        // If 'indexKeysToInclude' indicates that this index key component is not part of the
        // projection, remove it from the list of values that will be fed to the 'accessors'
        // list. Note that, even when we are excluding a key component, we can't skip the call
        // to 'KeyString::readSBEValue()' because it is needed to advance the 'reader' and
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
}  // namespace value
}  // namespace sbe
}  // namespace mongo
