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

#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo {
namespace sbe {
namespace bson {

/**
 * Advance table specifies how to change the pointer to skip current BSON value (so that pointer
 * points to the next byte after the BSON value):
 *  - For values less than 128 (0x80), pointer is advanced by this value
 *  - 255 (0xff) - pointer is advanced by the 32-bit integer stored in the buffer plus 4 bytes
 *  - 254 (0xfe) - pointer is advanced by the 32-bit integer stored in the buffer
 *  - 128 (0x80) - the type is either unsupported or handled explicitly
 */
// clang-format off
static uint8_t advanceTable[] = {
	0xff, // EOO
	8,    // Double
	0xff, // String
	0xfe, // Object
	0xfe, // Array
	0x80, // BinData
	0,    // Undefined - Deprecated
	12,   // ObjectId
	1,    // Boolean
	8,    // UTC datetime
	0,    // Null value
	0x80, // Regular expression
	0x80, // DBPointer - Deprecated
	0xff, // JavaScript code
	0xff, // Symbol - Deprecated
	0xfe, // JavaScript code with scope - Deprecated
	4,    // 32-bit integer
	8,    // Timestamp
	8,    // 64-bit integer
	16    // 128-bit decimal floating point

};
// clang-format on

const char* advance(const char* be, size_t fieldNameSize) {
    auto type = static_cast<unsigned char>(*be);

    be += 1 /*type*/ + fieldNameSize + 1 /*zero at the end of fieldname*/;
    if (type < sizeof(advanceTable)) {
        auto advOffset = advanceTable[type];
        if (advOffset < 128) {
            be += advOffset;
        } else if (static_cast<BSONType>(type) == BSONType::RegEx) {
            be += value::BsonRegex(be).byteSize();
        } else if (static_cast<BSONType>(type) == BSONType::DBRef) {
            be += value::BsonDBPointer(be).byteSize();
        } else {
            be += ConstDataView(be).read<LittleEndian<uint32_t>>();
            if (advOffset == 0xff) {
                be += 4;
            } else if (advOffset == 0xfe) {
            } else {
                if (static_cast<BSONType>(type) == BSONType::BinData) {
                    be += 5;
                } else {
                    uasserted(4822803, "unsupported bson element");
                }
            }
        }
    } else if (type == static_cast<unsigned char>(BSONType::MinKey) ||
               type == static_cast<unsigned char>(BSONType::MaxKey)) {
        // We don't have to adjust the 'be' pointer as the above types have no value part.
    } else {
        uasserted(4822804, "unsupported bson element");
    }

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
        case BSONType::NumberDouble: {
            double dbl = ConstDataView(be).read<LittleEndian<double>>();
            return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(dbl)};
        }
        case BSONType::NumberDecimal: {
            if constexpr (View) {
                return {value::TypeTags::NumberDecimal, value::bitcastFrom<const char*>(be)};
            }

            return value::makeCopyDecimal(value::readDecimal128FromMemory(ConstDataView{be}));
        }
        case BSONType::String: {
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
        case BSONType::Symbol: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonSymbol, value};
            }
            return value::makeNewBsonSymbol(
                value::getStringOrSymbolView(value::TypeTags::bsonSymbol, value));
        }
        case BSONType::BinData: {
            if constexpr (View) {
                return {value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(be)};
            }

            auto size = ConstDataView(be).read<LittleEndian<uint32_t>>();
            auto metaSize = sizeof(uint32_t) + 1;
            auto binData = new uint8_t[size + metaSize];
            memcpy(binData, be, size + metaSize);
            return {value::TypeTags::bsonBinData, value::bitcastFrom<uint8_t*>(binData)};
        }
        case BSONType::Object: {
            if constexpr (View) {
                return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(be)};
            }
            // Skip document length.
            be += 4;
            auto [tag, val] = value::makeNewObject();
            auto obj = value::getObjectView(val);

            while (*be != 0) {
                auto sv = bson::fieldNameView(be);

                auto [tag, val] = convertFrom<false>(be, end, sv.size());
                obj->push_back(sv, tag, val);

                be = advance(be, sv.size());
            }
            return {tag, val};
        }
        case BSONType::Array: {
            if constexpr (View) {
                return {value::TypeTags::bsonArray, value::bitcastFrom<const char*>(be)};
            }
            // Skip array length.
            be += 4;
            auto [tag, val] = value::makeNewArray();
            auto arr = value::getArrayView(val);

            while (*be != 0) {
                auto sv = bson::fieldNameView(be);

                auto [tag, val] = convertFrom<false>(be, end, sv.size());
                arr->push_back(tag, val);

                be = advance(be, sv.size());
            }
            return {tag, val};
        }
        case BSONType::jstOID: {
            if constexpr (View) {
                return {value::TypeTags::bsonObjectId, value::bitcastFrom<const char*>(be)};
            }
            auto [tag, val] = value::makeNewObjectId();
            memcpy(value::getObjectIdView(val), be, sizeof(value::ObjectIdType));
            return {tag, val};
        }
        case BSONType::Bool:
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(*(be))};
        case BSONType::Date: {
            int64_t integer = ConstDataView(be).read<LittleEndian<int64_t>>();
            return {value::TypeTags::Date, value::bitcastFrom<int64_t>(integer)};
        }
        case BSONType::jstNULL:
            return {value::TypeTags::Null, 0};
        case BSONType::NumberInt: {
            int32_t integer = ConstDataView(be).read<LittleEndian<int32_t>>();
            return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(integer)};
        }
        case BSONType::bsonTimestamp: {
            uint64_t val = ConstDataView(be).read<LittleEndian<uint64_t>>();
            return {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(val)};
        }
        case BSONType::NumberLong: {
            int64_t val = ConstDataView(be).read<LittleEndian<int64_t>>();
            return {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(val)};
        }
        case BSONType::MinKey:
            return {value::TypeTags::MinKey, 0};
        case BSONType::MaxKey:
            return {value::TypeTags::MaxKey, 0};
        case BSONType::Undefined:
            return {value::TypeTags::bsonUndefined, 0};
        case BSONType::RegEx: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonRegex, value};
            }
            return value::makeCopyBsonRegex(value::getBsonRegexView(value));
        }
        case BSONType::Code: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonJavascript, value};
            }
            return value::makeCopyBsonJavascript(value::getBsonJavascriptView(value));
        }
        case BSONType::DBRef: {
            auto value = value::bitcastFrom<const char*>(be);
            if constexpr (View) {
                return {value::TypeTags::bsonDBPointer, value};
            }
            return value::makeCopyBsonDBPointer(value::getBsonDBPointerView(value));
        }
        case BSONType::CodeWScope: {
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
void convertToBsonObj(ArrayBuilder& builder, value::ArrayEnumerator arr) {
    for (; !arr.atEnd(); arr.advance()) {
        auto [tag, val] = arr.getViewOfValue();

        switch (tag) {
            case value::TypeTags::Nothing:
                break;
            case value::TypeTags::NumberInt32:
                builder.append(value::bitcastTo<int32_t>(val));
                break;
            case value::TypeTags::RecordId:
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
            case value::TypeTags::Array: {
                ArrayBuilder subarrBuilder(builder.subarrayStart());
                convertToBsonObj(subarrBuilder, value::ArrayEnumerator{tag, val});
                subarrBuilder.doneFast();
                break;
            }
            case value::TypeTags::ArraySet: {
                ArrayBuilder subarrBuilder(builder.subarrayStart());
                convertToBsonObj(subarrBuilder, value::ArrayEnumerator{tag, val});
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
}

// Explicit instantiations
template void convertToBsonObj<BSONArrayBuilder>(BSONArrayBuilder&, value::ArrayEnumerator);
template void convertToBsonObj<UniqueBSONArrayBuilder>(UniqueBSONArrayBuilder&,
                                                       value::ArrayEnumerator);

template <class ArrayBuilder>
void convertToBsonObj(ArrayBuilder& builder, value::Array* arr) {
    return convertToBsonObj(
        builder,
        value::ArrayEnumerator{value::TypeTags::Array, value::bitcastFrom<value::Array*>(arr)});
}

template void convertToBsonObj<BSONArrayBuilder>(BSONArrayBuilder& builder, value::Array* arr);
template void convertToBsonObj<UniqueBSONArrayBuilder>(UniqueBSONArrayBuilder& builder,
                                                       value::Array* arr);

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
        case value::TypeTags::RecordId:
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
        case value::TypeTags::Array: {
            typename ObjBuilder::ArrayBuilder subarrBuilder(builder.subarrayStart(name));
            convertToBsonObj(subarrBuilder, value::ArrayEnumerator{tag, val});
            subarrBuilder.doneFast();
            break;
        }
        case value::TypeTags::ArraySet: {
            typename ObjBuilder::ArrayBuilder subarrBuilder(builder.subarrayStart(name));
            convertToBsonObj(subarrBuilder, value::ArrayEnumerator{tag, val});
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
