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

#include "mongo/db/exec/sbe/values/slot.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/bufreader.h"

namespace mongo::sbe::value {
static std::pair<TypeTags, Value> deserializeValue(BufReader& buf) {
    auto tag = static_cast<TypeTags>(buf.read<uint8_t>());
    Value val;

    switch (tag) {
        case TypeTags::Nothing:
            break;
        case TypeTags::NumberInt32:
            val = bitcastFrom<int32_t>(buf.read<LittleEndian<int32_t>>());
            break;
        case TypeTags::RecordId:
            std::tie(tag, val) = makeCopyRecordId(RecordId::deserializeToken(buf));
            break;
        case TypeTags::NumberInt64:
            val = bitcastFrom<int64_t>(buf.read<LittleEndian<int64_t>>());
            break;
        case TypeTags::NumberDouble:
            val = bitcastFrom<double>(buf.read<LittleEndian<double>>());
            break;
        case TypeTags::NumberDecimal: {
            uint64_t low = buf.read<LittleEndian<uint64_t>>();
            uint64_t high = buf.read<LittleEndian<uint64_t>>();
            auto [decTag, decVal] = makeCopyDecimal(Decimal128{Decimal128::Value{low, high}});
            val = decVal;
            break;
        }
        case TypeTags::Date:
            val = bitcastFrom<int64_t>(buf.read<LittleEndian<int64_t>>());
            break;
        case TypeTags::Timestamp:
            val = bitcastFrom<uint64_t>(buf.read<LittleEndian<uint64_t>>());
            break;
        case TypeTags::Boolean:
            val = bitcastFrom<bool>(buf.read<char>());
            break;
        case TypeTags::Null:
        case TypeTags::MinKey:
        case TypeTags::MaxKey:
        case TypeTags::bsonUndefined:
            val = 0;
            break;
        case TypeTags::StringSmall: {
            std::tie(tag, val) = makeNewString(buf.readCStr());
            break;
        }
        case TypeTags::StringBig:
        case TypeTags::bsonString: {
            auto stringLength = buf.read<LittleEndian<uint32_t>>();
            auto stringStart = reinterpret_cast<const char*>(buf.skip(stringLength));
            std::tie(tag, val) = makeNewString({stringStart, stringLength});
            break;
        }
        case TypeTags::bsonSymbol: {
            auto descriptionLength = buf.read<LittleEndian<uint32_t>>();
            auto descriptionStart = reinterpret_cast<const char*>(buf.skip(descriptionLength));
            std::tie(tag, val) = makeNewBsonSymbol({descriptionStart, descriptionLength});
            break;
        }
        case TypeTags::Array: {
            auto cnt = buf.read<LittleEndian<size_t>>();
            auto [arrTag, arrVal] = makeNewArray();
            auto arr = getArrayView(arrVal);
            if (cnt) {
                arr->reserve(cnt);
                for (size_t idx = 0; idx < cnt; ++idx) {
                    auto [tag, val] = deserializeValue(buf);
                    arr->push_back(tag, val);
                }
            }
            tag = arrTag;
            val = arrVal;
            break;
        }
        case TypeTags::ArraySet: {
            auto cnt = buf.read<LittleEndian<size_t>>();
            auto [arrTag, arrVal] = makeNewArraySet();
            auto arr = getArraySetView(arrVal);
            if (cnt) {
                arr->reserve(cnt);
                for (size_t idx = 0; idx < cnt; ++idx) {
                    auto [tag, val] = deserializeValue(buf);
                    arr->push_back(tag, val);
                }
            }
            tag = arrTag;
            val = arrVal;
            break;
        }
        case TypeTags::Object: {
            auto cnt = buf.read<LittleEndian<size_t>>();
            auto [objTag, objVal] = makeNewObject();
            auto obj = getObjectView(objVal);
            if (cnt) {
                obj->reserve(cnt);
                for (size_t idx = 0; idx < cnt; ++idx) {
                    auto fieldName = buf.readCStr();
                    auto [tag, val] = deserializeValue(buf);
                    obj->push_back(fieldName, tag, val);
                }
            }
            tag = objTag;
            val = objVal;
            break;
        }
        case TypeTags::bsonObjectId:
        case TypeTags::ObjectId: {
            auto [objIdTag, objIdVal] = makeNewObjectId();
            auto objId = getObjectIdView(objIdVal);
            buf.read(*objId);
            tag = objIdTag;
            val = objIdVal;
            break;
        }
        case TypeTags::bsonObject:
        case TypeTags::bsonArray: {
            auto size = buf.peek<LittleEndian<uint32_t>>();
            auto buffer = UniqueBuffer::allocate(size);
            memcpy(buffer.get(), buf.skip(size), size);
            return {tag, bitcastFrom<char*>(buffer.release())};
        }
        case TypeTags::bsonBinData: {
            auto binDataSize = buf.peek<LittleEndian<uint32_t>>();
            auto size = binDataSize + sizeof(uint32_t) + 1;
            auto binData = new uint8_t[size];
            memcpy(binData, buf.skip(size), size);
            val = bitcastFrom<uint8_t*>(binData);
            break;
        }
        case TypeTags::ksValue: {
            auto version = static_cast<KeyString::Version>(buf.read<uint8_t>());
            auto ks = KeyString::Value::deserialize(buf, version);
            auto [ksTag, ksVal] = makeCopyKeyString(ks);
            tag = ksTag;
            val = ksVal;
            break;
        }
        case TypeTags::bsonRegex: {
            auto pattern = buf.readCStr();
            auto flags = buf.readCStr();
            std::tie(tag, val) = value::makeNewBsonRegex(pattern, flags);
            break;
        }
        case TypeTags::bsonJavascript: {
            auto codeLength = buf.read<LittleEndian<uint32_t>>();
            auto codeStart = reinterpret_cast<const char*>(buf.skip(codeLength));
            std::tie(tag, val) = makeCopyBsonJavascript({codeStart, codeLength});
            break;
        }
        case TypeTags::bsonDBPointer: {
            auto nsLen = buf.read<LittleEndian<uint32_t>>();
            auto nsStart = reinterpret_cast<const char*>(buf.skip(nsLen));
            auto id = reinterpret_cast<const uint8_t*>(buf.skip(sizeof(ObjectIdType)));
            std::tie(tag, val) = makeNewBsonDBPointer({nsStart, nsLen}, id);
            break;
        }
        case TypeTags::bsonCodeWScope: {
            auto codeLen = buf.read<LittleEndian<uint32_t>>();
            auto codeStart = reinterpret_cast<const char*>(buf.skip(codeLen));
            auto scopeLen = buf.peek<LittleEndian<uint32_t>>();
            auto scope = reinterpret_cast<const char*>(buf.skip(scopeLen));
            std::tie(tag, val) = makeNewBsonCodeWScope({codeStart, codeLen}, scope);
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }

    return {tag, val};
}

MaterializedRow MaterializedRow::deserializeForSorter(BufReader& buf,
                                                      const SorterDeserializeSettings&) {
    auto cnt = buf.read<LittleEndian<size_t>>();
    MaterializedRow result{cnt};

    for (size_t idx = 0; idx < cnt; ++idx) {
        auto [tag, val] = deserializeValue(buf);
        result.reset(idx, true, tag, val);
    }

    return result;
}

static void serializeValue(BufBuilder& buf, TypeTags tag, Value val) {
    buf.appendUChar(static_cast<uint8_t>(tag));

    switch (tag) {
        case TypeTags::Nothing:
            break;
        case TypeTags::NumberInt32:
            buf.appendNum(bitcastTo<int32_t>(val));
            break;
        case TypeTags::RecordId:
            getRecordIdView(val)->serializeToken(buf);
            break;
        case TypeTags::NumberInt64:
            buf.appendNum(bitcastTo<int64_t>(val));
            break;
        case TypeTags::NumberDouble:
            buf.appendNum(bitcastTo<double>(val));
            break;
        case TypeTags::NumberDecimal:
            buf.appendNum(value::bitcastTo<Decimal128>(val));
            break;
        case TypeTags::Date:
            buf.appendNum(bitcastTo<int64_t>(val));
            break;
        case TypeTags::Timestamp:
            buf.appendNum(bitcastTo<uint64_t>(val));
            break;
        case TypeTags::Boolean:
            buf.appendNum(static_cast<char>(bitcastTo<bool>(val)));
            break;
        case TypeTags::Null:
            break;
        case TypeTags::MinKey:
            break;
        case TypeTags::MaxKey:
            break;
        case TypeTags::bsonUndefined:
            break;
        case TypeTags::StringSmall: {
            // Small strings cannot contain null bytes, so it is safe to serialize them as plain
            // C-strings with a null terminator.
            buf.appendStr(getStringView(tag, val), true /* includeEndingNull */);
            break;
        }
        case TypeTags::StringBig:
        case TypeTags::bsonString:
        case TypeTags::bsonSymbol: {
            auto sv = getStringOrSymbolView(tag, val);
            buf.appendNum(static_cast<uint32_t>(sv.size()));
            buf.appendStr(sv, false /* includeEndingNull */);
            break;
        }
        case TypeTags::Array: {
            auto arr = getArrayView(val);
            buf.appendNum(arr->size());
            for (size_t idx = 0; idx < arr->size(); ++idx) {
                auto [tag, val] = arr->getAt(idx);
                serializeValue(buf, tag, val);
            }
            break;
        }
        case TypeTags::ArraySet: {
            auto arr = getArraySetView(val);
            buf.appendNum(arr->size());
            for (auto& kv : arr->values()) {
                serializeValue(buf, kv.first, kv.second);
            }
            break;
        }
        case TypeTags::Object: {
            auto obj = getObjectView(val);
            buf.appendNum(obj->size());
            for (size_t idx = 0; idx < obj->size(); ++idx) {
                buf.appendStr(obj->field(idx), true /* includeEndingNull */);
                auto [tag, val] = obj->getAt(idx);
                serializeValue(buf, tag, val);
            }
            break;
        }
        case TypeTags::ObjectId: {
            auto objId = getObjectIdView(val);
            buf.appendStruct(*objId);
            break;
        }
        case TypeTags::bsonObject: {
            auto bson = getRawPointerView(val);
            auto size = ConstDataView(bson).read<LittleEndian<uint32_t>>();
            buf.appendBuf(bson, size);
            break;
        }
        case TypeTags::bsonArray: {
            auto arr = getRawPointerView(val);
            auto size = ConstDataView(arr).read<LittleEndian<uint32_t>>();
            buf.appendBuf(arr, size);
            break;
        }
        case TypeTags::bsonObjectId: {
            auto objId = getRawPointerView(val);
            buf.appendBuf(objId, sizeof(ObjectIdType));
            break;
        }
        case TypeTags::bsonBinData: {
            auto binData = getRawPointerView(val);
            auto size = getBSONBinDataSize(tag, val);
            buf.appendNum(static_cast<uint32_t>(size));
            buf.appendBuf(binData + sizeof(uint32_t), size + 1);
            break;
        }
        case TypeTags::ksValue: {
            auto ks = getKeyStringView(val);
            buf.appendUChar(static_cast<uint8_t>(ks->getVersion()));
            ks->serialize(buf);
            break;
        }
        case TypeTags::bsonRegex: {
            auto regex = getBsonRegexView(val);
            buf.appendStr(regex.pattern, true /* includeEndingNull */);
            buf.appendStr(regex.flags, true /* includeEndingNull */);
            break;
        }
        case TypeTags::bsonJavascript: {
            auto javascriptCode = getBsonJavascriptView(val);
            buf.appendNum(static_cast<uint32_t>(javascriptCode.size()));
            buf.appendStr(javascriptCode, false /* includeEndingNull */);
            break;
        }
        case TypeTags::bsonDBPointer: {
            auto dbptr = getBsonDBPointerView(val);
            buf.appendNum(static_cast<uint32_t>(dbptr.ns.size()));
            buf.appendStr(dbptr.ns, false /* includeEndingNull */);
            buf.appendBuf(dbptr.id, sizeof(ObjectIdType));
            break;
        }
        case TypeTags::bsonCodeWScope: {
            auto cws = getBsonCodeWScopeView(val);
            buf.appendNum(static_cast<uint32_t>(cws.code.size()));
            buf.appendStr(cws.code, false /* includeEndingNull */);
            auto scopeLen = ConstDataView(cws.scope).read<LittleEndian<uint32_t>>();
            buf.appendBuf(cws.scope, scopeLen);
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

static void serializeValueIntoKeyString(KeyString::Builder& buf, TypeTags tag, Value val) {
    switch (tag) {
        case TypeTags::Nothing: {
            buf.appendBool(false);
            break;
        }
        case TypeTags::NumberInt32: {
            buf.appendBool(true);
            buf.appendNumberInt(bitcastTo<int32_t>(val));
            break;
        }
        case TypeTags::NumberInt64: {
            buf.appendBool(true);
            buf.appendNumberLong(bitcastTo<int64_t>(val));
            break;
        }
        case TypeTags::NumberDouble: {
            buf.appendBool(true);
            buf.appendNumberDouble(bitcastTo<double>(val));
            break;
        }
        case TypeTags::NumberDecimal: {
            buf.appendBool(true);
            buf.appendNumberDecimal(value::bitcastTo<Decimal128>(val));
            break;
        }
        case TypeTags::Date: {
            buf.appendBool(true);
            buf.appendDate(Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(val)));
            break;
        }
        case TypeTags::Timestamp: {
            buf.appendBool(true);
            buf.appendTimestamp(Timestamp(value::bitcastTo<uint64_t>(val)));
            break;
        }
        case TypeTags::Boolean: {
            buf.appendBool(true);
            buf.appendBool(bitcastTo<bool>(val));
            break;
        }
        case TypeTags::Null: {
            buf.appendBool(true);
            buf.appendNull();
            break;
        }
        case TypeTags::MinKey:
        case TypeTags::MaxKey: {
            BSONObjBuilder bob;
            if (tag == value::TypeTags::MinKey) {
                bob.appendMinKey("");
            } else {
                bob.appendMaxKey("");
            }
            buf.appendBool(true);
            buf.appendBSONElement(bob.obj().firstElement(), nullptr);
            break;
        }
        case TypeTags::bsonUndefined: {
            buf.appendBool(true);
            buf.appendUndefined();
            break;
        }
        case TypeTags::StringSmall: {
            // Small strings cannot contain null bytes, so it is safe to serialize them as plain
            // C-strings with a null terminator.
            buf.appendBool(true);
            buf.appendString(getStringView(tag, val));
            break;
        }
        case TypeTags::StringBig:
        case TypeTags::bsonString: {
            buf.appendBool(true);
            buf.appendString(getStringOrSymbolView(tag, val));
            break;
        }
        case TypeTags::bsonSymbol: {
            buf.appendBool(true);
            buf.appendSymbol(getStringOrSymbolView(tag, val));
            break;
        }
        case TypeTags::ArraySet:
        case TypeTags::Array: {
            // TODO SERVER-61629: convert this to serialize the 'arr' directly instead of
            // constructing a BSONArray.
            BSONArrayBuilder builder;
            bson::convertToBsonObj(builder, getArrayView(val));
            buf.appendBool(true);
            buf.appendArray(BSONArray(builder.done()));
            break;
        }
        case TypeTags::Object: {
            // TODO SERVER-61629: convert this to serialize the 'obj' directly instead of
            // constructing a BSONObj.
            BSONObjBuilder builder;
            bson::convertToBsonObj(builder, getObjectView(val));
            buf.appendBool(true);
            buf.appendObject(builder.done());
            break;
        }
        case TypeTags::ObjectId: {
            buf.appendBool(true);
            buf.appendBytes(getObjectIdView(val), sizeof(ObjectIdType));
            break;
        }
        case TypeTags::bsonObject: {
            buf.appendBool(true);
            buf.appendObject(BSONObj(getRawPointerView(val)));
            break;
        }
        case TypeTags::bsonArray: {
            buf.appendBool(true);
            buf.appendArray(BSONArray(BSONObj(getRawPointerView(val))));
            break;
        }
        case TypeTags::bsonObjectId: {
            buf.appendBool(true);
            buf.appendOID(OID::from(getRawPointerView(val)));
            break;
        }
        case TypeTags::bsonBinData: {
            BufBuilder innerBinDataBuf;
            innerBinDataBuf.appendUChar(static_cast<uint8_t>(tag));
            innerBinDataBuf.appendBuf(getRawPointerView(val),
                                      getBSONBinDataSize(tag, val) + sizeof(uint32_t) + 1);
            buf.appendBool(true);
            buf.appendBinData(
                BSONBinData(innerBinDataBuf.buf(), innerBinDataBuf.len(), BinDataGeneral));
            break;
        }
        case TypeTags::bsonRegex: {
            auto regex = getBsonRegexView(val);
            buf.appendBool(true);
            buf.appendRegex(BSONRegEx(regex.pattern, regex.flags));
            break;
        }
        case TypeTags::bsonJavascript: {
            buf.appendBool(true);
            buf.appendCode(getBsonJavascriptView(val));
            break;
        }
        case TypeTags::bsonDBPointer: {
            auto dbptr = getBsonDBPointerView(val);
            buf.appendBool(true);
            buf.appendDBRef(BSONDBRef(dbptr.ns, OID::from(dbptr.id)));
            break;
        }
        case TypeTags::bsonCodeWScope: {
            auto cws = getBsonCodeWScopeView(val);
            buf.appendBool(true);
            buf.appendCodeWString(BSONCodeWScope(cws.code, BSONObj(cws.scope)));
            break;
        }
        case TypeTags::ksValue: {
            auto ks = getKeyStringView(val);
            BufBuilder innerBinDataBuf;
            innerBinDataBuf.appendUChar(static_cast<uint8_t>(tag));
            ks->serialize(innerBinDataBuf);
            buf.appendBool(true);
            buf.appendBinData(
                BSONBinData(innerBinDataBuf.buf(), innerBinDataBuf.len(), BinDataGeneral));
            break;
        }
        case TypeTags::RecordId: {
            BufBuilder innerBinDataBuf;
            innerBinDataBuf.appendUChar(static_cast<uint8_t>(tag));
            getRecordIdView(val)->serializeToken(innerBinDataBuf);
            buf.appendBool(true);
            buf.appendBinData(BSONBinData(
                innerBinDataBuf.buf(), innerBinDataBuf.len(), BinDataType::BinDataGeneral));
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

void MaterializedRow::serializeForSorter(BufBuilder& buf) const {
    buf.appendNum(size());

    for (size_t idx = 0; idx < size(); ++idx) {
        auto [tag, val] = getViewOfValue(idx);
        serializeValue(buf, tag, val);
    }
}

void MaterializedRow::serializeIntoKeyString(KeyString::Builder& buf) const {
    for (size_t idx = 0; idx < size(); ++idx) {
        auto [tag, val] = getViewOfValue(idx);
        serializeValueIntoKeyString(buf, tag, val);
    }
}

MaterializedRow MaterializedRow::deserializeFromKeyString(const KeyString::Value& keyString,
                                                          BufBuilder* valueBufferBuilder) {
    BufReader reader(keyString.getBuffer(), keyString.getSize());
    KeyString::TypeBits typeBits(keyString.getTypeBits());
    KeyString::TypeBits::Reader typeBitsReader(typeBits);

    MaterializedRowValueBuilder valBuilder(valueBufferBuilder);
    auto keepReading = true;
    do {
        keepReading = KeyString::readSBEValue(
            &reader, &typeBitsReader, false /* inverted */, typeBits.version, &valBuilder);
    } while (keepReading);

    MaterializedRow result{valBuilder.numValues()};
    valBuilder.readValues(result);

    return result;
}

int getApproximateSize(TypeTags tag, Value val) {
    int result = sizeof(tag) + sizeof(val);
    switch (tag) {
        // These are shallow types.
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
            break;
        // There are deep types.
        case TypeTags::RecordId:
            result += getRecordIdView(val)->memUsage();
            break;
        case TypeTags::NumberDecimal:
            result += sizeof(Decimal128);
            break;
        case TypeTags::StringBig:
        case TypeTags::bsonString: {
            result += sizeof(uint32_t) + getStringLength(tag, val) + sizeof(char);
            break;
        }
        case TypeTags::bsonSymbol:
            result += sizeof(uint32_t) + getStringOrSymbolView(tag, val).size() + sizeof(char);
            break;
        case TypeTags::Array: {
            auto arr = getArrayView(val);
            result += sizeof(*arr);
            for (size_t idx = 0; idx < arr->size(); ++idx) {
                auto [tag, val] = arr->getAt(idx);
                result += getApproximateSize(tag, val);
            }
            break;
        }
        case TypeTags::ArraySet: {
            auto arr = getArraySetView(val);
            result += sizeof(*arr);
            for (auto& kv : arr->values()) {
                result += getApproximateSize(kv.first, kv.second);
            }
            break;
        }
        case TypeTags::Object: {
            auto obj = getObjectView(val);
            result += sizeof(*obj);
            for (size_t idx = 0; idx < obj->size(); ++idx) {
                result += obj->field(idx).size();
                auto [tag, val] = obj->getAt(idx);
                result += getApproximateSize(tag, val);
            }
            break;
        }
        case TypeTags::ObjectId:
        case TypeTags::bsonObjectId:
            result += sizeof(ObjectIdType);
            break;
        case TypeTags::bsonObject:
        case TypeTags::bsonArray: {
            auto ptr = getRawPointerView(val);
            result += ConstDataView(ptr).read<LittleEndian<uint32_t>>();
            break;
        }
        case TypeTags::bsonBinData:
            // The 32-bit 'length' at the beginning of a BinData does _not_ account for the
            // 'length' field itself or the 'subtype' field, so we account for that here.
            result += sizeof(uint32_t) + sizeof(char) +
                ConstDataView(getRawPointerView(val)).read<LittleEndian<uint32_t>>();
            break;
        case TypeTags::ksValue: {
            auto ks = getKeyStringView(val);
            result += ks->getSize();
            break;
        }
        case TypeTags::bsonRegex: {
            auto regex = getBsonRegexView(val);
            result += regex.byteSize();
            break;
        }
        case TypeTags::bsonJavascript: {
            auto code = getBsonJavascriptView(val);
            result += sizeof(uint32_t) + code.size() + sizeof(char);
            break;
        }
        case TypeTags::bsonDBPointer:
            result += getBsonDBPointerView(val).byteSize();
            break;
        case TypeTags::bsonCodeWScope:
            // CodeWScope's 'length' field accounts for the full length of the CodeWScope
            // including the 'length' field itself.
            result += ConstDataView(getRawPointerView(val)).read<LittleEndian<uint32_t>>();
            break;
        case TypeTags::pcreRegex:
            result += getPcreRegexView(val)->codeSize();
            break;
        case TypeTags::timeZoneDB:
            // This type points to a block of memory that it doesn't own, so we don't acccount
            // for the size of this block of memory here.
            break;
        case TypeTags::jsFunction:
            result += getJsFunctionView(val)->getApproximateSize();
            break;
        case TypeTags::shardFilterer:
            result += getShardFiltererView(val)->getApproximateSize();
            break;
        case TypeTags::collator:
            // This type points to a block of memory that it doesn't own, so we don't acccount
            // for the size of this block of memory here.
            break;
        case TypeTags::ftsMatcher:
            result += getFtsMatcherView(val)->getApproximateSize();
            break;
        case TypeTags::sortSpec:
            result += getSortSpecView(val)->getApproximateSize();
            break;
        case TypeTags::makeObjSpec:
            result += getMakeObjSpecView(val)->getApproximateSize();
            break;
        case TypeTags::indexBounds:
            result += size_estimator::estimate(*getIndexBoundsView(val));
            break;
        default:
            MONGO_UNREACHABLE;
    }
    return result;
}

int MaterializedRow::memUsageForSorter() const {
    int result = sizeof(MaterializedRow);

    for (size_t idx = 0; idx < size(); ++idx) {
        auto [tag, val] = getViewOfValue(idx);
        result += getApproximateSize(tag, val);
    }

    return result;
}

}  // namespace mongo::sbe::value
