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

#include "mongo/db/exec/sbe/values/bson.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <cstring>

namespace mongo {
namespace sbe {
namespace bson {
// clang-format off
const uint8_t kAdvanceTable alignas(64)[256] = {
    0x7F, // 0: EOO
    8,    // 1: Double
    0xFB, // 2: String
    0xFF, // 3: Object
    0xFF, // 4: Array
    0xFA, // 5: BinData
    0,    // 6: Undefined
    12,   // 7: ObjectId
    1,    // 8: Boolean
    8,    // 9: UTC datetime
    0,    // 10: Null
    0x7F, // 11: Regular expression
    0xEF, // 12: DBPointer
    0xFB, // 13: JavaScript code
    0xFB, // 14: Symbol
    0xFF, // 15: JavaScript code with scope
    4,    // 16: 32-bit integer
    8,    // 17: Timestamp
    8,    // 18: 64-bit integer
    16,   // 19: 128-bit decimal floating point
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 20-29:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 30-39:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 40-49:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 50-59:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 60-69:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 70-79:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 80-89:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 90-99:   Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 100-109: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 110-119: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,                   // 120-126: Invalid
    0,                                                          // 127:     MaxKey
    0x7F, 0x7F,                                                 // 128-129: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 130-139: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 140-149: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 150-159: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 160-169: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 170-179: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 180-189: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 190-199: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 200-209: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 210-219: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 220-229: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 230-239: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, // 240-249: Invalid
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F,                               // 250-254: Invalid
    0,                                                          // 255:     MinKey
};
// clang-format on

const char* advanceHelper(const char* be, size_t fieldNameSize) {
    auto type = static_cast<unsigned char>(*be);
    uassert(4822804, "unsupported bson element", static_cast<BSONType>(type) == BSONType::regEx);

    size_t sizeOfTypeCodeAndFieldName =
        1 /*type*/ + fieldNameSize + 1 /*zero at the end of fieldname*/;

    be += sizeOfTypeCodeAndFieldName;
    be += value::BsonRegex(be).byteSize();
    return be;
}

template <bool View>
std::pair<value::TypeTags, value::Value> convertFrom(const char* be,
                                                     const char* end,
                                                     size_t fieldNameSize) {
    auto type = static_cast<BSONType>(static_cast<signed char>(*be));
    // Advance the 'be' pointer;
    be += 1 + fieldNameSize + 1;

    switch (type) {
        case BSONType::numberDouble: {
            double dbl = ConstDataView(be).read<LittleEndian<double>>();
            return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(dbl)};
        }
        case BSONType::numberDecimal: {
            if constexpr (View) {
                return {value::TypeTags::NumberDecimal, value::bitcastFrom<const char*>(be)};
            }

            return value::makeCopyDecimal(value::readDecimal128FromMemory(ConstDataView{be}));
        }
        case BSONType::string: {
            if constexpr (View) {
                return {value::TypeTags::bsonString, value::bitcastFrom<const char*>(be)};
            }
            // len includes trailing zero.
            auto lenWithNull = uint32_t{ConstDataView(be).read<LittleEndian<uint32_t>>()};
            be += sizeof(lenWithNull);
            if (value::canUseSmallString({be, lenWithNull - 1})) {
                value::Value smallString;
                // Copy 8 bytes fast if we have space.
                if (be + 8 < end) {
                    memcpy(&smallString, be, 8);
                } else {
                    memcpy(&smallString, be, lenWithNull);
                }
                return {value::TypeTags::StringSmall, smallString};
            } else {
                return value::makeBigString({be, lenWithNull - 1});
            }
        }
        case BSONType::symbol: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonSymbol, value};
            }
            return value::makeNewBsonSymbol(
                value::getStringOrSymbolView(value::TypeTags::bsonSymbol, value));
        }
        case BSONType::binData: {
            if constexpr (View) {
                return {value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(be)};
            }

            auto size = ConstDataView(be).read<LittleEndian<uint32_t>>();
            auto metaSize = sizeof(uint32_t) + 1;
            auto binData = new uint8_t[size + metaSize];
            memcpy(binData, be, size + metaSize);
            return {value::TypeTags::bsonBinData, value::bitcastFrom<uint8_t*>(binData)};
        }
        case BSONType::object: {
            if constexpr (View) {
                return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(be)};
            }
            const auto objEnd = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
            be += 4;
            auto [tag, val] = value::makeNewObject();
            const auto obj = value::getObjectView(val);

            while (be != objEnd - 1) {
                auto sv = bson::fieldNameAndLength(be);

                auto [tag, val] = convertFrom<false>(be, end, sv.size());
                obj->push_back(sv, tag, val);

                be = advance(be, sv.size());
            }
            return {tag, val};
        }
        case BSONType::array: {
            if constexpr (View) {
                return {value::TypeTags::bsonArray, value::bitcastFrom<const char*>(be)};
            }
            // Skip array length.
            const auto arrEnd = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
            be += 4;
            auto [tag, val] = value::makeNewArray();
            auto arr = value::getArrayView(val);

            while (be != arrEnd - 1) {
                auto sv = bson::fieldNameAndLength(be);

                auto [tag, val] = convertFrom<false>(be, end, sv.size());
                arr->push_back(tag, val);

                be = advance(be, sv.size());
            }
            return {tag, val};
        }
        case BSONType::oid: {
            if constexpr (View) {
                return {value::TypeTags::bsonObjectId, value::bitcastFrom<const char*>(be)};
            }
            auto [tag, val] = value::makeNewObjectId();
            memcpy(value::getObjectIdView(val), be, sizeof(value::ObjectIdType));
            return {tag, val};
        }
        case BSONType::boolean:
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(*(be))};
        case BSONType::date: {
            int64_t integer = ConstDataView(be).read<LittleEndian<int64_t>>();
            return {value::TypeTags::Date, value::bitcastFrom<int64_t>(integer)};
        }
        case BSONType::null:
            return {value::TypeTags::Null, 0};
        case BSONType::numberInt: {
            int32_t integer = ConstDataView(be).read<LittleEndian<int32_t>>();
            return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(integer)};
        }
        case BSONType::timestamp: {
            uint64_t val = ConstDataView(be).read<LittleEndian<uint64_t>>();
            return {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(val)};
        }
        case BSONType::numberLong: {
            int64_t val = ConstDataView(be).read<LittleEndian<int64_t>>();
            return {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(val)};
        }
        case BSONType::minKey:
            return {value::TypeTags::MinKey, 0};
        case BSONType::maxKey:
            return {value::TypeTags::MaxKey, 0};
        case BSONType::undefined:
            return {value::TypeTags::bsonUndefined, 0};
        case BSONType::regEx: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonRegex, value};
            }
            return value::makeCopyBsonRegex(value::getBsonRegexView(value));
        }
        case BSONType::code: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonJavascript, value};
            }
            return value::makeCopyBsonJavascript(value::getBsonJavascriptView(value));
        }
        case BSONType::dbRef: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonDBPointer, value};
            }
            return value::makeCopyBsonDBPointer(value::getBsonDBPointerView(value));
        }
        case BSONType::codeWScope: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonCodeWScope, value};
            }
            return value::makeCopyBsonCodeWScope(value::getBsonCodeWScopeView(value));
        }
        default:
            return {value::TypeTags::Nothing, 0};
    }
}

template std::pair<value::TypeTags, value::Value> convertFrom<false>(const char* be,
                                                                     const char* end,
                                                                     size_t fieldNameSize);

template std::pair<value::TypeTags, value::Value> convertFrom<true>(const char* be,
                                                                    const char* end,
                                                                    size_t fieldNameSize);

template <class ArrayBuilder>
void convertToBsonArr(ArrayBuilder& builder, value::ArrayEnumerator arr) {
    for (; !arr.atEnd(); arr.advance()) {
        auto [tag, val] = arr.getViewOfValue();
        appendValueToBsonArr(builder, tag, val);
    }
}

// Explicit instantiations
template void convertToBsonArr<BSONArrayBuilder>(BSONArrayBuilder&, value::ArrayEnumerator);
template void convertToBsonArr<UniqueBSONArrayBuilder>(UniqueBSONArrayBuilder&,
                                                       value::ArrayEnumerator);

template <class ArrayBuilder>
void convertToBsonArr(ArrayBuilder& builder, value::Array* arr) {
    return convertToBsonArr(
        builder,
        value::ArrayEnumerator{value::TypeTags::Array, value::bitcastFrom<value::Array*>(arr)});
}

template void convertToBsonArr<BSONArrayBuilder>(BSONArrayBuilder& builder, value::Array* arr);
template void convertToBsonArr<UniqueBSONArrayBuilder>(UniqueBSONArrayBuilder& builder,
                                                       value::Array* arr);

template <class ArrayBuilder>
void appendValueToBsonArr(ArrayBuilder& builder, value::TypeTags tag, value::Value val) {
    switch (tag) {
        case value::TypeTags::Nothing:
            break;
        case value::TypeTags::NumberInt32:
            builder.append(value::bitcastTo<int32_t>(val));
            break;
        case value::TypeTags::NumberInt64:
            builder.append(value::bitcastTo<int64_t>(val));
            break;
        case value::TypeTags::NumberDouble:
            builder.append(value::bitcastTo<double>(val));
            break;
        case value::TypeTags::NumberDecimal:
            builder.append(value::bitcastTo<Decimal128>(val));
            break;
        case value::TypeTags::Date:
            builder.append(Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(val)));
            break;
        case value::TypeTags::Timestamp:
            builder.append(Timestamp(value::bitcastTo<uint64_t>(val)));
            break;
        case value::TypeTags::Boolean:
            builder.append(value::bitcastTo<bool>(val));
            break;
        case value::TypeTags::Null:
            builder.appendNull();
            break;
        case value::TypeTags::StringSmall:
        case value::TypeTags::StringBig:
        case value::TypeTags::bsonString:
            builder.append(value::getStringView(tag, val));
            break;
        case value::TypeTags::bsonSymbol:
            builder.append(BSONSymbol{value::getStringOrSymbolView(tag, val)});
            break;
        case value::TypeTags::Array:
        case value::TypeTags::ArraySet:
        case value::TypeTags::ArrayMultiSet: {
            ArrayBuilder subarrBuilder(builder.subarrayStart());
            convertToBsonArr(subarrBuilder, value::ArrayEnumerator{tag, val});
            subarrBuilder.doneFast();
            break;
        }
        case value::TypeTags::Object: {
            typename ArrayBuilder::ObjBuilder subobjBuilder(builder.subobjStart());
            convertToBsonObj(subobjBuilder, value::getObjectView(val));
            subobjBuilder.doneFast();
            break;
        }
        case value::TypeTags::ObjectId:
            builder.append(OID::from(value::getObjectIdView(val)->data()));
            break;
        case value::TypeTags::MinKey:
            builder.appendMinKey();
            break;
        case value::TypeTags::MaxKey:
            builder.appendMaxKey();
            break;
        case value::TypeTags::bsonObject:
            builder.append(BSONObj{value::bitcastTo<const char*>(val)});
            break;
        case value::TypeTags::bsonArray:
            builder.append(BSONArray{BSONObj{value::bitcastTo<const char*>(val)}});
            break;
        case value::TypeTags::bsonObjectId:
            builder.append(OID::from(value::bitcastTo<const char*>(val)));
            break;
        case value::TypeTags::bsonBinData:
            // BinData is also subject to the bson size limit, so the cast here is safe.
            builder.append(BSONBinData{value::getBSONBinData(tag, val),
                                       static_cast<int>(value::getBSONBinDataSize(tag, val)),
                                       getBSONBinDataSubtype(tag, val)});
            break;
        case value::TypeTags::bsonUndefined:
            builder.appendUndefined();
            break;
        case value::TypeTags::bsonRegex: {
            auto regex = value::getBsonRegexView(val);
            builder.appendRegex(regex.pattern, regex.flags);
            break;
        }
        case value::TypeTags::bsonJavascript:
            builder.appendCode(value::getBsonJavascriptView(val));
            break;
        case value::TypeTags::bsonDBPointer: {
            auto dbptr = value::getBsonDBPointerView(val);
            builder.append(BSONDBRef{dbptr.ns, OID::from(dbptr.id)});
            break;
        }
        case value::TypeTags::bsonCodeWScope: {
            auto cws = value::getBsonCodeWScopeView(val);
            builder.append(BSONCodeWScope{cws.code, BSONObj(cws.scope)});
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

template <class ObjBuilder>
void convertToBsonObj(ObjBuilder& builder, value::Object* obj) {
    for (size_t idx = 0; idx < obj->size(); ++idx) {
        auto [tag, val] = obj->getAt(idx);
        const auto& name = obj->field(idx);
        appendValueToBsonObj(builder, name, tag, val);
    }
}

template void convertToBsonObj<BSONObjBuilder>(BSONObjBuilder& builder, value::Object* obj);
template void convertToBsonObj<UniqueBSONObjBuilder>(UniqueBSONObjBuilder& builder,
                                                     value::Object* obj);

template <class ObjBuilder>
void appendValueToBsonObj(ObjBuilder& builder,
                          StringData name,
                          value::TypeTags tag,
                          value::Value val) {
    switch (tag) {
        case value::TypeTags::Nothing:
            break;
        case value::TypeTags::NumberInt32:
            builder.append(name, value::bitcastTo<int32_t>(val));
            break;
        case value::TypeTags::NumberInt64:
            builder.append(name, value::bitcastTo<int64_t>(val));
            break;
        case value::TypeTags::NumberDouble:
            builder.append(name, value::bitcastTo<double>(val));
            break;
        case value::TypeTags::NumberDecimal:
            builder.append(name, value::bitcastTo<Decimal128>(val));
            break;
        case value::TypeTags::Date:
            builder.append(name, Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(val)));
            break;
        case value::TypeTags::Timestamp:
            builder.append(name, Timestamp(value::bitcastTo<uint64_t>(val)));
            break;
        case value::TypeTags::Boolean:
            builder.append(name, value::bitcastTo<bool>(val));
            break;
        case value::TypeTags::Null:
            builder.appendNull(name);
            break;
        case value::TypeTags::StringSmall:
        case value::TypeTags::StringBig:
        case value::TypeTags::bsonString:
            builder.append(name, value::getStringView(tag, val));
            break;
        case value::TypeTags::bsonSymbol:
            builder.appendSymbol(name, value::getStringOrSymbolView(tag, val));
            break;
        case value::TypeTags::Array:
        case value::TypeTags::ArraySet:
        case value::TypeTags::ArrayMultiSet: {
            typename ObjBuilder::ArrayBuilder subarrBuilder(builder.subarrayStart(name));
            convertToBsonArr(subarrBuilder, value::ArrayEnumerator{tag, val});
            subarrBuilder.doneFast();
            break;
        }
        case value::TypeTags::Object: {
            ObjBuilder subobjBuilder(builder.subobjStart(name));
            convertToBsonObj(subobjBuilder, value::getObjectView(val));
            subobjBuilder.doneFast();
            break;
        }
        case value::TypeTags::ObjectId:
            builder.append(name, OID::from(value::getObjectIdView(val)->data()));
            break;
        case value::TypeTags::MinKey:
            builder.appendMinKey(name);
            break;
        case value::TypeTags::MaxKey:
            builder.appendMaxKey(name);
            break;
        case value::TypeTags::bsonObject:
            builder.appendObject(name, value::bitcastTo<const char*>(val));
            break;
        case value::TypeTags::bsonArray:
            builder.appendArray(name, BSONObj{value::bitcastTo<const char*>(val)});
            break;
        case value::TypeTags::bsonObjectId:
            builder.append(name, OID::from(value::bitcastTo<const char*>(val)));
            break;
        case value::TypeTags::bsonBinData: {
            builder.appendBinData(name,
                                  value::getBSONBinDataSize(tag, val),
                                  value::getBSONBinDataSubtype(tag, val),
                                  value::getBSONBinData(tag, val));
            break;
        }
        case value::TypeTags::bsonUndefined:
            builder.appendUndefined(name);
            break;
        case value::TypeTags::bsonRegex: {
            auto regex = value::getBsonRegexView(val);
            builder.appendRegex(name, regex.pattern, regex.flags);
            break;
        }
        case value::TypeTags::bsonJavascript:
            builder.appendCode(name, value::getBsonJavascriptView(val));
            break;
        case value::TypeTags::bsonDBPointer: {
            auto dbptr = value::getBsonDBPointerView(val);
            builder.appendDBRef(name, dbptr.ns, OID::from(dbptr.id));
            break;
        }
        case value::TypeTags::bsonCodeWScope: {
            auto cws = value::getBsonCodeWScopeView(val);
            builder.appendCodeWScope(name, cws.code, BSONObj(cws.scope));
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

template void appendValueToBsonArr<BSONArrayBuilder>(BSONArrayBuilder& builder,
                                                     value::TypeTags tag,
                                                     value::Value val);
template void appendValueToBsonArr<UniqueBSONArrayBuilder>(UniqueBSONArrayBuilder& builder,
                                                           value::TypeTags tag,
                                                           value::Value val);

template void appendValueToBsonObj<BSONObjBuilder>(BSONObjBuilder& builder,
                                                   StringData name,
                                                   value::TypeTags tag,
                                                   value::Value val);
template void appendValueToBsonObj<UniqueBSONObjBuilder>(UniqueBSONObjBuilder& builder,
                                                         StringData name,
                                                         value::TypeTags tag,
                                                         value::Value val);
}  // namespace bson
}  // namespace sbe
}  // namespace mongo
