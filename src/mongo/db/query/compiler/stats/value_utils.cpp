/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/stats/value_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <fmt/format.h>

namespace mongo::stats {

/**
 * Generates and returns a sorted vector of all type tags.
 *
 * This function enumerates all possible type tags defined in the sbe::value::TypeTags enum. It then
 * sorts these type tags using the compareStrictOrder() function. The compareStrictOrder() function
 * sorts according to their sort order. If two type tags share the same sort order, it breaks the
 * tie using the TypeTags enum values.
 */
std::vector<sbe::value::TypeTags> sortTypeTags();

/**
 * Generates and returns a map of type tags to their respective subsequent type tags.
 *
 * This function creates a mapping from each type tag to its subsequent type tag, determined by the
 * order of type tags in the 'kTypeTagsSorted' vector. If two type tags share the same canonical
 * order, they will map to the same subsequent type tag.
 *
 * The function skips the last type tag in the vector, as there is no subsequent type tag for it.
 */
absl::flat_hash_map<sbe::value::TypeTags, sbe::value::TypeTags> nextTypeTagsMap();

namespace {

const static std::vector<sbe::value::TypeTags> kTypeTagsSorted = sortTypeTags();

const static absl::flat_hash_map<sbe::value::TypeTags, sbe::value::TypeTags> kNextTypeTagsMap =
    nextTypeTagsMap();

}  // namespace

namespace value = sbe::value;

SBEValue::SBEValue(value::TypeTags tag, value::Value val) : _tag(tag), _val(val) {}

SBEValue::SBEValue(std::pair<value::TypeTags, value::Value> v) : SBEValue(v.first, v.second) {}

SBEValue::SBEValue(const SBEValue& other) {
    auto [tag, val] = copyValue(other._tag, other._val);
    _tag = tag;
    _val = val;
}

SBEValue::SBEValue(SBEValue&& other) {
    _tag = other._tag;
    _val = other._val;

    other._tag = value::TypeTags::Nothing;
    other._val = 0;
}

SBEValue::~SBEValue() {
    value::releaseValue(_tag, _val);
}

SBEValue& SBEValue::operator=(const SBEValue& other) {
    auto [tag, val] = copyValue(other._tag, other._val);
    value::releaseValue(_tag, _val);

    _tag = tag;
    _val = val;
    return *this;
}

SBEValue& SBEValue::operator=(SBEValue&& other) {
    value::releaseValue(_tag, _val);

    _tag = other._tag;
    _val = other._val;

    other._tag = value::TypeTags::Nothing;
    other._val = 0;

    return *this;
}

std::pair<value::TypeTags, value::Value> SBEValue::get() const {
    return std::make_pair(_tag, _val);
}

value::TypeTags SBEValue::getTag() const {
    return _tag;
}

value::Value SBEValue::getValue() const {
    return _val;
}

std::pair<value::TypeTags, value::Value> makeBooleanValue(int64_t v) {
    return std::make_pair(value::TypeTags::Boolean, value::bitcastFrom<bool>(v));
};

std::pair<value::TypeTags, value::Value> makeInt64Value(int64_t v) {
    return std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
};

std::pair<value::TypeTags, value::Value> makeInt32Value(int32_t v) {
    return std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(v));
};

std::pair<value::TypeTags, value::Value> makeDoubleValue(double v) {
    return std::make_pair(value::TypeTags::NumberDouble, value::bitcastFrom<double>(v));
}

std::pair<value::TypeTags, value::Value> makeDateValue(Date_t v) {
    return std::make_pair(value::TypeTags::Date,
                          value::bitcastFrom<int64_t>(v.toMillisSinceEpoch()));
}

std::pair<value::TypeTags, value::Value> makeTimestampValue(Timestamp v) {
    return std::make_pair(value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(v.asULL()));
}

std::pair<value::TypeTags, value::Value> makeNullValue() {
    return std::make_pair(value::TypeTags::Null, 0);
};

std::pair<value::TypeTags, value::Value> makeNaNValue() {
    return std::make_pair(
        sbe::value::TypeTags::NumberDouble,
        sbe::value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
};

bool sameTypeClass(value::TypeTags tag1, value::TypeTags tag2) {
    return compareTypeTags(tag1, tag2) == 0;
}

bool sameTypeBracket(value::TypeTags tag1, value::TypeTags tag2) {
    if (tag1 == tag2) {
        return true;
    }
    return ((value::isNumber(tag1) && value::isNumber(tag2)) ||
            (value::isString(tag1) && value::isString(tag2)));
}

int32_t compareValues(value::TypeTags tag1,
                      value::Value val1,
                      value::TypeTags tag2,
                      value::Value val2) {
    const auto [compareTag, compareVal] = value::compareValue(tag1, val1, tag2, val2);
    uassert(6660547, "Invalid comparison result", compareTag == value::TypeTags::NumberInt32);
    return value::bitcastTo<int32_t>(compareVal);
}

bool isEmptyArray(value::TypeTags tag, value::Value val) {
    auto [tagArray, valEmptyArray] = value::makeNewArray();
    auto isEmpty = (stats::compareValues(tag, val, tagArray, valEmptyArray) == 0);
    value::releaseValue(tagArray, valEmptyArray);
    return isEmpty;
}

bool isTrueBool(value::TypeTags tag, value::Value val) {
    return (stats::compareValues(tag,
                                 val,
                                 sbe::value::TypeTags::Boolean,
                                 sbe::value::bitcastFrom<int64_t>(1) /*SBEValue boolean*/) == 0);
}

void sortValueVector(std::vector<SBEValue>& sortVector) {
    const auto cmp = [](const SBEValue& a, const SBEValue& b) {
        return compareValues(a.getTag(), a.getValue(), b.getTag(), b.getValue()) < 0;
    };
    std::sort(sortVector.begin(), sortVector.end(), cmp);
}

template <typename T, typename C, size_t E>
double convertToDouble(const T& arr, const size_t maxPrecision) {
    double result = 0.0;
    for (size_t i = 0; i < maxPrecision; ++i) {
        const C ch = arr[i];
        const double charToDbl = ch / std::pow(2, i * E);
        result += charToDbl;
    }
    return result;
}

double stringToDouble(StringData sd) {
    constexpr size_t exponent = sizeof(double);
    const size_t maxPrecision = std::min(sd.size(), exponent);
    return convertToDouble<StringData, char, exponent>(sd, maxPrecision);
}

double objectIdToDouble(const value::ObjectIdType* oid) {
    // An ObjectId is backed by an array of 12 unsigned characters, therefore we can treat it as a
    // string and apply the same formula to convert it to a double while ensuring that the double
    // value is sorted lexicographically. This is necessary because valueSpread() expects the double
    // value to be ordered in the same way as the values used to generate a histogram.
    constexpr size_t maxPrecision = sizeof(value::ObjectIdType);
    return convertToDouble<value::ObjectIdType, uint8_t, maxPrecision>(*oid, maxPrecision);
}

double valueToDouble(value::TypeTags tag, value::Value val) {
    double result = 0;
    if (tag == value::TypeTags::NumberDecimal) {
        // We cannot directly cast NumberDecimal values to doubles, because they are wider. However,
        // we can downcast a Decimal128 type with rounding to a double value.
        const Decimal128 d = value::numericCast<Decimal128>(tag, val);
        result = d.toDouble();

    } else if (value::isNumber(tag)) {
        result = value::numericCast<double>(tag, val);

    } else if (value::isString(tag)) {
        const StringData sd = value::getStringView(tag, val);
        result = stringToDouble(sd);

    } else if (tag == value::TypeTags::Date) {
        int64_t v = value::bitcastTo<int64_t>(val);
        result = value::numericCast<double>(value::TypeTags::NumberInt64, v);

    } else if (tag == value::TypeTags::Timestamp) {
        uint64_t v = value::bitcastTo<uint64_t>(val);
        result = double(v);

    } else if (tag == value::TypeTags::ObjectId) {
        const auto oid = sbe::value::getObjectIdView(val);
        result = objectIdToDouble(oid);

    } else {
        uassert(6844500, "Unexpected value type", false);
    }

    return result;
}

void addSbeValueToBSONArrayBuilder(const SBEValue& sbeValue, BSONArrayBuilder& builder) {

    switch (sbeValue.getTag()) {
        case sbe::value::TypeTags::NumberInt32:
            builder.append(
                value::numericCast<int>(sbe::value::TypeTags::NumberInt32, sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::NumberInt64:
            builder.append(value::numericCast<long long>(sbe::value::TypeTags::NumberInt64,
                                                         sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::NumberDecimal:
            builder.append(value::numericCast<Decimal128>(sbe::value::TypeTags::NumberDecimal,
                                                          sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::NumberDouble:
            builder.append(
                value::numericCast<double>(value::TypeTags::NumberInt64, sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::StringSmall:
        case sbe::value::TypeTags::StringBig:
        case sbe::value::TypeTags::bsonString:
            builder.append(value::getStringView(sbeValue.getTag(), sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::Boolean:
            builder.appendBool(sbe::value::bitcastTo<bool>(sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::Null:
            builder.appendNull();
            break;
        default:
            uassert(9370101, "Unexpected value type", false);
            break;
    }
}

void addSbeValueToBSONBuilder(const SBEValue& sbeValue,
                              const std::string& fieldName,
                              BSONObjBuilder& builder) {

    switch (sbeValue.getTag()) {
        case sbe::value::TypeTags::NumberInt32:
            builder.appendNumber(
                fieldName,
                value::numericCast<int>(sbe::value::TypeTags::NumberInt32, sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::NumberInt64:
            builder.appendNumber(fieldName,
                                 value::numericCast<long long>(sbe::value::TypeTags::NumberInt64,
                                                               sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::NumberDecimal:
            builder.appendNumber(fieldName,
                                 value::numericCast<Decimal128>(sbe::value::TypeTags::NumberDecimal,
                                                                sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::NumberDouble:
            builder.appendNumber(
                fieldName,
                value::numericCast<double>(value::TypeTags::NumberInt64, sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::StringSmall:
        case sbe::value::TypeTags::StringBig:
        case sbe::value::TypeTags::bsonString:
            builder.append(fieldName, value::getStringView(sbeValue.getTag(), sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::Boolean:
            builder.appendBool(fieldName, sbe::value::bitcastTo<bool>(sbeValue.getValue()));
            break;
        case sbe::value::TypeTags::Null:
            builder.appendNull(fieldName);
            break;
        case sbe::value::TypeTags::Array: {
            BSONArrayBuilder bsonArrayBuilder;
            value::Array* arr = value::getArrayView(sbeValue.getValue());
            size_t arrSize = arr->size();
            for (size_t i = 0; i < arrSize; i++) {
                addSbeValueToBSONArrayBuilder(SBEValue(arr->getAt(i)), bsonArrayBuilder);
            }
            builder.appendArray(fieldName, bsonArrayBuilder.obj());
            break;
        }
        default:
            uassert(9370100, "Unexpected value type", false);
            break;
    }
}

BSONObj sbeValueVectorToBSON(std::vector<SBEValue>& sbeValues,
                             std::vector<std::string>& fieldNames) {
    BSONObjBuilder builder;

    for (size_t idx = 0; idx < fieldNames.size(); idx++) {
        addSbeValueToBSONBuilder(sbeValues[idx], fieldNames[idx], builder);
    }

    return builder.obj();
}

BSONObj sbeValueToBSON(const SBEValue& sbeValue, const std::string& fieldName) {
    BSONObjBuilder builder;

    addSbeValueToBSONBuilder(sbeValue, fieldName, builder);

    return builder.obj();
}

BSONObj sbeValuesToInterval(const SBEValue& sbeValueLow,
                            const std::string& fieldNameLow,
                            const SBEValue& sbeValueHigh,
                            const std::string& fieldNameHigh) {

    BSONObjBuilder builder;

    addSbeValueToBSONBuilder(sbeValueLow, fieldNameLow, builder);
    addSbeValueToBSONBuilder(sbeValueHigh, fieldNameHigh, builder);

    return builder.obj();
}

bool canEstimateTypeViaHistogram(value::TypeTags tag) {
    if (sbe::value::isNumber(tag) || value::isString(tag)) {
        return true;
    }

    switch (tag) {
        // Other types that we can/do build histograms on:
        // - Date/time types.
        case value::TypeTags::Date:
        case value::TypeTags::Timestamp:
        // - ObjectId.
        case value::TypeTags::ObjectId:
            return true;

        default:
            return false;
    }

    MONGO_UNREACHABLE;
}

bool canEstimateTypeViaTypeCounts(sbe::value::TypeTags tag) {
    switch (tag) {
        case sbe::value::TypeTags::Boolean:
            // There are dedicated counters for true and false, making it always estimable.
        case sbe::value::TypeTags::Null:
        case sbe::value::TypeTags::MinKey:
        case sbe::value::TypeTags::MaxKey:
        case sbe::value::TypeTags::bsonUndefined:
            // These types have a single possible value, allowing cardinality estimation from type
            // counts.
            return true;
        default:
            return false;
    }
}

bool canEstimateIntervalViaTypeCounts(sbe::value::TypeTags startTag,
                                      sbe::value::Value startVal,
                                      bool startInclusive,
                                      sbe::value::TypeTags endTag,
                                      sbe::value::Value endVal,
                                      bool endInclusive) {

    bool isFullBracketInterval = stats::isFullBracketInterval(
        startTag, startVal, startInclusive, endTag, endVal, endInclusive);

    if (isFullBracketInterval) {
        return true;
    }

    bool sameTypeBracketInterval =
        stats::sameTypeBracketInterval(startTag, endInclusive, endTag, endVal);

    bool pointQuery = (sameTypeBracketInterval &&
                       (stats::compareValues(startTag, startVal, endTag, endVal) == 0));

    // Types that can be estimated via the type-counters.
    if (sameTypeBracketInterval && canEstimateTypeViaTypeCounts(startTag)) {
        return true;
    }

    switch (startTag) {
        case value::TypeTags::Array: {
            if (sameTypeBracketInterval && stats::isEmptyArray(startTag, startVal) && pointQuery) {
                return true;
            }
            break;
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberDouble: {
            if (sameTypeBracketInterval && sbe::value::isNaN(startTag, startVal) && pointQuery) {
                return true;
            }
            break;
        }
        default:
            return false;
    }

    return false;
}

std::string serialize(value::TypeTags tag) {
    std::ostringstream os;
    os << tag;
    return os.str();
}

// TODO: does this belong in SBE value utils?
value::TypeTags deserialize(const std::string& name) {
    if ("NumberInt32" == name) {
        return value::TypeTags::NumberInt32;
    } else if ("NumberInt64" == name) {
        return value::TypeTags::NumberInt64;
    } else if ("NumberDecimal" == name) {
        return value::TypeTags::NumberDecimal;
    } else if ("NumberDouble" == name) {
        return value::TypeTags::NumberDouble;
    } else if ("StringBig" == name) {
        return value::TypeTags::StringBig;
    } else if ("StringSmall" == name) {
        return value::TypeTags::StringSmall;
    } else if ("bsonString" == name) {
        return value::TypeTags::bsonString;
    } else if ("Date" == name) {
        return value::TypeTags::Date;
    } else if ("Timestamp" == name) {
        return value::TypeTags::Timestamp;
    } else if ("ObjectId" == name) {
        return value::TypeTags::ObjectId;
    } else if ("Object" == name) {
        return value::TypeTags::Object;
    } else if ("Boolean" == name) {
        return value::TypeTags::Boolean;
    } else if ("Array" == name) {
        return value::TypeTags::Array;
    } else if ("Null" == name) {
        return value::TypeTags::Null;
    } else if ("bsonUndefined" == name) {
        return value::TypeTags::bsonUndefined;
    } else if ("bsonJavascript" == name) {
        return value::TypeTags::bsonJavascript;
    } else if ("bsonBinData" == name) {
        return value::TypeTags::bsonBinData;
    } else if ("bsonRegex" == name) {
        return value::TypeTags::bsonRegex;
    } else if ("MinKey" == name) {
        return value::TypeTags::MinKey;
    } else if ("MaxKey" == name) {
        return value::TypeTags::MaxKey;
    } else if ("Nothing" == name) {
        return value::TypeTags::Nothing;
    }

    // Trying to deserialize any other types should result in an error.
    uasserted(6660600,
              str::stream() << "String " << name << " is not convertable to SBE type tag.");
}

int compareTypeTags(sbe::value::TypeTags a, sbe::value::TypeTags b) {
    auto orderOfA = canonicalizeBSONTypeUnsafeLookup(tagToType(a));
    auto orderOfB = canonicalizeBSONTypeUnsafeLookup(tagToType(b));
    if (orderOfA < orderOfB) {
        return -1;
    } else if (orderOfA > orderOfB) {
        return 1;
    }
    return 0;
}


std::vector<sbe::value::TypeTags> sortTypeTags() {
    // Sorts type tags according to the sort order. Breaks tie with the TypeTags enum values if
    // two tags share the same sort order.
    auto compareStrictOrder = [](sbe::value::TypeTags a, sbe::value::TypeTags b) -> bool {
        auto result = compareTypeTags(a, b);
        return result != 0 ? result < 0 : (a < b);
    };

    static constexpr size_t numTypeTags = size_t(sbe::value::TypeTags::TypeTagsMax);
    std::vector<sbe::value::TypeTags> typeTagsOrder;

    // Enumerates all the type tags.
    for (uint8_t tagValue = 0; tagValue < numTypeTags; ++tagValue) {
        auto tag = static_cast<sbe::value::TypeTags>(tagValue);

        // Skips unsupported types.
        if (sbe::value::tagToType(tag) == BSONType::eoo) {
            continue;
        }
        typeTagsOrder.push_back(tag);
    }

    std::sort(typeTagsOrder.begin(), typeTagsOrder.end(), compareStrictOrder);

    return typeTagsOrder;
}

absl::flat_hash_map<sbe::value::TypeTags, sbe::value::TypeTags> nextTypeTagsMap() {
    absl::flat_hash_map<sbe::value::TypeTags, sbe::value::TypeTags> nextTypeTagsMap;

    // Skips the last one as there is no next type.
    sbe::value::TypeTags nextTag = kTypeTagsSorted[kTypeTagsSorted.size() - 1];
    invariant(!kTypeTagsSorted.empty());
    for (int32_t index = kTypeTagsSorted.size() - 2; index >= 0; --index) {
        auto tag = kTypeTagsSorted[index];
        nextTypeTagsMap[tag] = nextTag;

        // If 'tag' and 'kTypeTagsSorted[index - 1]' are at the same canonical order, reuse
        // 'nextTag'. For example, as compareTypeTags(NumberInt32, NumberDouble) == 0, their next
        // type tags are both 'StringSmall'.
        if (!(index > 0 && compareTypeTags(tag, kTypeTagsSorted[index - 1]) == 0)) {
            nextTag = tag;
        }
    }

    return nextTypeTagsMap;
}

sbe::value::TypeTags getNextType(sbe::value::TypeTags tag) {
    auto it = kNextTypeTagsMap.find(tag);
    tassert(9619600,
            fmt::format("Type {} does not have a next type", fmt::underlying(tag)),
            it != kNextTypeTagsMap.end());
    return it->second;
}

bool isVariableWidthType(sbe::value::TypeTags tag) {
    return isVariableWidthType(tagToType(tag));
}

std::pair<stats::SBEValue, bool> getMinBound(sbe::value::TypeTags tag) {
    switch (tag) {
        case sbe::value::TypeTags::MinKey:
            return {{tag, 0}, true};
        case sbe::value::TypeTags::MaxKey:
            return {{tag, 0}, true};
        case sbe::value::TypeTags::NumberInt32:
        case sbe::value::TypeTags::NumberInt64:
        case sbe::value::TypeTags::NumberDouble:
        case sbe::value::TypeTags::NumberDecimal:
            return {{sbe::value::TypeTags::NumberDouble,
                     sbe::value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
                    true};

        case sbe::value::TypeTags::StringSmall:
        case sbe::value::TypeTags::StringBig:
        case sbe::value::TypeTags::bsonString:
        case sbe::value::TypeTags::bsonSymbol:
            return {sbe::value::makeNewString(""), true};

        case sbe::value::TypeTags::Date:
            return {{tag, sbe::value::bitcastFrom<int64_t>(Date_t::min().toMillisSinceEpoch())},
                    true};

        case sbe::value::TypeTags::Timestamp:
            return {{tag, sbe::value::bitcastFrom<uint64_t>(Timestamp::min().asULL())}, true};

        case sbe::value::TypeTags::Null:
            return {{tag, 0}, true};

        case sbe::value::TypeTags::bsonUndefined: {
            return {sbe::value::makeValue(Value(BSONUndefined)), true};
        }

        case sbe::value::TypeTags::Object:
        case sbe::value::TypeTags::bsonObject:
            return {sbe::value::makeNewObject(), true};

        case sbe::value::TypeTags::Array:
        case sbe::value::TypeTags::ArraySet:
        case sbe::value::TypeTags::ArrayMultiSet:
        case sbe::value::TypeTags::bsonArray:
            return {sbe::value::makeNewArray(), true};

        case sbe::value::TypeTags::bsonBinData:
            return {sbe::value::makeValue(Value(BSONBinData())), true};

        case sbe::value::TypeTags::Boolean:
            return {{tag, sbe::value::bitcastFrom<bool>(false)}, true};

        case sbe::value::TypeTags::ObjectId:
        case sbe::value::TypeTags::bsonObjectId:
            return {sbe::value::makeValue(Value(OID())), true};

        case sbe::value::TypeTags::bsonRegex:
            return {sbe::value::makeValue(Value(BSONRegEx("", ""))), true};

        case sbe::value::TypeTags::bsonDBPointer:
            return {sbe::value::makeValue(Value(BSONDBRef())), true};

        case sbe::value::TypeTags::bsonJavascript:
            return {sbe::value::makeCopyBsonJavascript(StringData("")), true};

        case sbe::value::TypeTags::bsonCodeWScope:
            return {sbe::value::makeValue(Value(BSONCodeWScope())), true};
        default:
            tasserted(9619601, str::stream() << "Type not supported for getMinBound: " << tag);
    }

    MONGO_UNREACHABLE_TASSERT(9619602);
}

std::pair<stats::SBEValue, bool> getMaxBound(sbe::value::TypeTags tag) {
    // If the type is a variable width type, the maximum value cannot be represented with the same
    // type. Therefore, we use the minimum value of the next type to represent the maximum bound.
    // The inclusive flag is set to false to indicate that the bound is excluded.
    if (isVariableWidthType(tag)) {
        auto bound = getMinBound(getNextType(tag));
        bound.second = false;
        return bound;
    }

    switch (tag) {
        case sbe::value::TypeTags::MinKey:
            return {{tag, 0}, true};
        case sbe::value::TypeTags::MaxKey:
            return {{tag, 0}, true};
        case sbe::value::TypeTags::NumberInt32:
        case sbe::value::TypeTags::NumberInt64:
        case sbe::value::TypeTags::NumberDouble:
        case sbe::value::TypeTags::NumberDecimal:
            return {{sbe::value::TypeTags::NumberDouble,
                     sbe::value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
                    true};
        case sbe::value::TypeTags::Date:
            return {{tag, sbe::value::bitcastFrom<int64_t>(Date_t::max().toMillisSinceEpoch())},
                    true};
        case sbe::value::TypeTags::Timestamp:
            return {{tag, sbe::value::bitcastFrom<uint64_t>(Timestamp::max().asULL())}, true};

        case sbe::value::TypeTags::Null:
            return {{tag, 0}, true};

        case sbe::value::TypeTags::bsonUndefined:
            return {sbe::value::makeValue(Value(BSONUndefined)), true};

        case sbe::value::TypeTags::Boolean:
            return {{tag, sbe::value::bitcastFrom<bool>(true)}, true};
        case sbe::value::TypeTags::ObjectId:
        case sbe::value::TypeTags::bsonObjectId:
            return {sbe::value::makeValue(Value(OID::max())), true};
        default:
            tasserted(9619603, str::stream() << "Type not supported for getMaxBound: " << tag);
    }

    MONGO_UNREACHABLE_TASSERT(9619604);
}

std::string printInterval(bool startInclusive,
                          sbe::value::TypeTags startTag,
                          sbe::value::Value startVal,
                          bool endInclusive,
                          sbe::value::TypeTags endTag,
                          sbe::value::Value endVal) {
    return str::stream() << (startInclusive ? "[" : "(") << std::pair(startTag, startVal) << ", "
                         << std::pair(endTag, endVal) << (endInclusive ? "]" : ")");
}

bool sameTypeBracketInterval(sbe::value::TypeTags startTag,
                             bool endInclusive,
                             sbe::value::TypeTags endTag,
                             sbe::value::Value endVal) {
    if (stats::sameTypeClass(startTag, endTag)) {
        return true;
    }

    if (endInclusive) {
        return false;
    }

    auto [max, maxInclusive] = getMinBound(getNextType(startTag));
    return stats::compareValues(endTag, endVal, max.getTag(), max.getValue()) == 0;
}

bool isFullBracketInterval(sbe::value::TypeTags startTag,
                           sbe::value::Value startVal,
                           bool startInclusive,
                           sbe::value::TypeTags endTag,
                           sbe::value::Value endVal,
                           bool endInclusive) {
    // 'startInclusive' must be true because a full bracket interval includes the minimum value of
    // the type.
    if (!startInclusive) {
        return false;
    }

    // Short-circuits by first evaluating 'endInclusive'. This approach prevents unnecessary memory
    // allocation by avoiding calls to getMinBound() and getMaxBound() if the conditions are not
    // met.
    //
    // The logic checks if the start and end tags are of the same type. If they are, either
    // isVariableWidthType(startTag) and (!endInclusive) must be false for the interval to be
    // considered a full bracket interval.
    //
    // Example scenarios:
    // - If 'startTag' is Object, the logic returns false Because isVariableWidthType() is true.
    //   We expect the end bound of a full bracket interval of object to be an empty Array.
    // - If 'startTag' is NumberInt32, the logic returns false if 'endInclusive' is false, as we
    //   expect the end bound to be infinitiy and inclusive.
    bool sameType = sameTypeClass(startTag, endTag);
    if (sameType && (isVariableWidthType(startTag) || !endInclusive)) {
        return false;
    } else if (!sameType && (endInclusive || !sameTypeClass(endTag, getNextType(startTag)))) {
        // If the start and end bounds are of different type classes, 'endInclusive' must be false
        // and 'endTag' must be the next type of 'startTag' for the interval to be considered a full
        // bracket interval.
        //
        // Example scenario:
        // - If 'startTag' is Object, 'endTag' must be Array (or equivalent) and 'endInclusive'
        //   must be false for the interval to be valid.
        return false;
    }

    auto [expectedMin, minInclusive] = getMinBound(startTag);
    auto [expectedMax, maxInclusive] =
        sameType ? getMaxBound(startTag) : getMinBound(getNextType(startTag));

    bool compareValuesMin =
        (stats::compareValues(startTag, startVal, expectedMin.getTag(), expectedMin.getValue()) ==
         0);

    bool compareValuesMax =
        (stats::compareValues(endTag, endVal, expectedMax.getTag(), expectedMax.getValue()) == 0);

    return compareValuesMin && compareValuesMax;
}

bool isEmptyInterval(sbe::value::TypeTags startTag,
                     sbe::value::Value startVal,
                     bool startInclusive,
                     sbe::value::TypeTags endTag,
                     sbe::value::Value endVal,
                     bool endInclusive) {
    return compareValues(startTag, startVal, endTag, endVal) == 0 &&
        !(startInclusive && endInclusive);
}

std::vector<std::pair<std::pair<SBEValue, bool>, std::pair<SBEValue, bool>>> bracketizeInterval(
    sbe::value::TypeTags startTag,
    sbe::value::Value startVal,
    bool startInclusive,
    sbe::value::TypeTags endTag,
    sbe::value::Value endVal,
    bool endInclusive) {
    std::vector<std::pair<std::pair<SBEValue, bool>, std::pair<SBEValue, bool>>> intervals;

    // Skips if the interval is empty.
    if (isEmptyInterval(startTag, startVal, startInclusive, endTag, endVal, endInclusive)) {
        return intervals;
    }

    // Short-circuits if the interval is of the same type.
    if (sameTypeBracketInterval(startTag, endInclusive, endTag, endVal)) {
        auto start = std::pair(SBEValue(copyValue(startTag, startVal)), startInclusive);
        auto end = std::pair(SBEValue(copyValue(endTag, endVal)), endInclusive);
        intervals.emplace_back(std::move(start), std::move(end));
        return intervals;
    }

    // At this point, the interval is a mixed-type interval. We will start bracketizing it.
    // Note: 'Nothing' is not included in 'kTypeTagsSorted', so it's safe as a dummy.
    sbe::value::TypeTags prevTag = sbe::value::TypeTags::Nothing;
    for (auto tag : kTypeTagsSorted) {
        if (compareTypeTags(tag, startTag) < 0) {
            continue;
        }

        // Ends searching when the rest of the tags are greater then 'endTag'.
        if (compareTypeTags(tag, endTag) > 0) {
            break;
        }

        // Dedup the type tags with the same order such as NumberInt32 and NumberDouble.
        if (sameTypeClass(tag, prevTag)) {
            continue;
        }
        prevTag = tag;

        std::pair<SBEValue, bool> start = sameTypeClass(tag, startTag)
            ? std::pair(SBEValue(copyValue(startTag, startVal)), startInclusive)
            : getMinBound(tag);

        std::pair<SBEValue, bool> end = sameTypeClass(tag, endTag)
            ? std::pair(SBEValue(copyValue(endTag, endVal)), endInclusive)
            : getMaxBound(tag);

        // Skips if the interval is empty.
        if (isEmptyInterval(start.first.getTag(),
                            start.first.getValue(),
                            start.second,
                            end.first.getTag(),
                            end.first.getValue(),
                            end.second)) {
            continue;
        }

        intervals.emplace_back(std::move(start), std::move(end));
    }
    return intervals;
}

}  // namespace mongo::stats
