// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/value_size.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::sbe::value {

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
        case TypeTags::LocalOneArgLambda:
        case TypeTags::LocalTwoArgLambda:
            return result;
        // There are deep types.
        case TypeTags::RecordId:
            result += getRecordIdView(val)->memUsage();
            return result;
        case TypeTags::NumberDecimal:
            result += sizeof(Decimal128);
            return result;
        case TypeTags::StringBig:
        case TypeTags::bsonString: {
            result += sizeof(uint32_t) + getStringLength(tag, val) + sizeof(char);
            return result;
        }
        case TypeTags::bsonSymbol:
            result += sizeof(uint32_t) + getStringOrSymbolView(tag, val).size() + sizeof(char);
            return result;
        case TypeTags::Array: {
            auto arr = getArrayView(val);
            result += sizeof(*arr);
            for (size_t idx = 0; idx < arr->size(); ++idx) {
                auto tagVal = arr->getAt(idx);
                result += getApproximateSize(tagVal.tag, tagVal.value);
            }
            return result;
        }
        case TypeTags::ArraySet: {
            auto arr = getArraySetView(val);
            result += sizeof(*arr);
            for (auto& kv : arr->values()) {
                result += getApproximateSize(kv.first, kv.second);
            }
            return result;
        }
        case TypeTags::ArrayMultiSet: {
            auto arr = getArrayMultiSetView(val);
            result += sizeof(*arr);
            for (auto& kv : arr->values()) {
                result += getApproximateSize(kv.first, kv.second);
            }
            return result;
        }
        case TypeTags::Object: {
            auto obj = getObjectView(val);
            result += sizeof(*obj);
            for (size_t idx = 0; idx < obj->size(); ++idx) {
                result += obj->field(idx).size();
                auto tagVal = obj->getAt(idx);
                result += getApproximateSize(tagVal.tag, tagVal.value);
            }
            return result;
        }
        case TypeTags::MultiMap: {
            auto multiMap = getMultiMapView(val);
            result += sizeof(*multiMap);
            for (auto& [key, value] : multiMap->values()) {
                result += getApproximateSize(key.first, key.second);
                result += getApproximateSize(value.first, value.second);
            }
            return result;
        }
        case TypeTags::ObjectId:
        case TypeTags::bsonObjectId:
            result += sizeof(ObjectIdType);
            return result;
        case TypeTags::bsonObject:
        case TypeTags::bsonArray: {
            auto ptr = getRawPointerView(val);
            result += ConstDataView(ptr).read<LittleEndian<uint32_t>>();
            return result;
        }
        case TypeTags::bsonBinData:
            // The 32-bit 'length' at the beginning of a BinData does _not_ account for the
            // 'length' field itself or the 'subtype' field, so we account for that here.
            result += sizeof(uint32_t) + sizeof(char) +
                ConstDataView(getRawPointerView(val)).read<LittleEndian<uint32_t>>();
            return result;
        case TypeTags::keyString: {
            auto ks = getKeyString(val);
            result += ks->getSerializedSize();
            return result;
        }
        case TypeTags::bsonRegex: {
            auto regex = getBsonRegexView(val);
            result += regex.byteSize();
            return result;
        }
        case TypeTags::bsonJavascript: {
            auto code = getBsonJavascriptView(val);
            result += sizeof(uint32_t) + code.size() + sizeof(char);
            return result;
        }
        case TypeTags::bsonDBPointer:
            result += getBsonDBPointerView(val).byteSize();
            return result;
        case TypeTags::bsonCodeWScope:
            // CodeWScope's 'length' field accounts for the full length of the CodeWScope
            // including the 'length' field itself.
            result += ConstDataView(getRawPointerView(val)).read<LittleEndian<uint32_t>>();
            return result;
        case TypeTags::timeZoneDB:
            // This type points to a block of memory that it doesn't own, so we don't acccount
            // for the size of this block of memory here.
            return result;
        case TypeTags::timeZone:
            // The timezone obj stores an offset counter, and a pointer to a timelib struct
            // which it doesn't own, so we don't need to account for the timelib obj.
            result += sizeof(TimeZone);
            return result;
        case TypeTags::collator:
        case TypeTags::inList:
            // This type points to a block of memory that it doesn't own, so we don't account
            // for the size of this block of memory here.
            return result;
        case TypeTags::pcreRegex:
        case TypeTags::jsFunction:
        case TypeTags::shardFilterer:
        case TypeTags::ftsMatcher:
        case TypeTags::sortSpec:
        case TypeTags::makeObjSpec:
        case TypeTags::indexBounds:
            result += getExtendedTypeOps(tag)->getApproximateSize(val);
            return result;
        case TypeTags::valueBlock:
            result += getValueBlock(val)->getApproximateSize();
            return result;
        case TypeTags::cellBlock:
            result += getCellBlock(val)->getApproximateSize();
            return result;
        case TypeTags::sortKeyComponentVector:
            result += sizeof(SortKeyComponentVector);
            result += getSortKeyComponentVectorView(val)->elts.capacity() *
                sizeof(std::pair<value::TypeTags, value::Value>);
            // Values inside the vector point to unowned memory, so we don't account for them here.
            return result;
        case TypeTags::TypeTagsMax:
            MONGO_UNREACHABLE_TASSERT(12725500);
    }
    tasserted(11122917, str::stream() << "Unkown type tag " << tag);
}

}  // namespace mongo::sbe::value
