// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/compare_numbers.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

#include <cmath>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
namespace value {
template <typename Op>
inline std::pair<TypeTags, Value> genericCompare(TypeTags lhsTag,
                                                 Value lhsValue,
                                                 TypeTags rhsTag,
                                                 Value rhsValue,
                                                 const StringDataComparator* comparator = nullptr,
                                                 Op op = {}) {
    if (isNumber(lhsTag) && isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case TypeTags::NumberInt32: {
                auto result = op(numericCast<int32_t>(lhsTag, lhsValue),
                                 numericCast<int32_t>(rhsTag, rhsValue));
                return {TypeTags::Boolean, bitcastFrom<bool>(result)};
            }
            case TypeTags::NumberInt64: {
                auto result = op(numericCast<int64_t>(lhsTag, lhsValue),
                                 numericCast<int64_t>(rhsTag, rhsValue));
                return {TypeTags::Boolean, bitcastFrom<bool>(result)};
            }
            case TypeTags::NumberDouble: {
                auto result = [&]() {
                    if (lhsTag == TypeTags::NumberInt64) {
                        auto rhs = bitcastTo<double>(rhsValue);
                        if (std::isnan(rhs)) {
                            return false;
                        }
                        return op(compareLongToDouble(bitcastTo<int64_t>(lhsValue), rhs), 0);
                    } else if (rhsTag == TypeTags::NumberInt64) {
                        auto lhs = bitcastTo<double>(lhsValue);
                        if (std::isnan(lhs)) {
                            return false;
                        }
                        return op(compareDoubleToLong(lhs, bitcastTo<int64_t>(rhsValue)), 0);
                    } else {
                        return op(numericCast<double>(lhsTag, lhsValue),
                                  numericCast<double>(rhsTag, rhsValue));
                    }
                }();
                return {TypeTags::Boolean, bitcastFrom<bool>(result)};
            }
            case TypeTags::NumberDecimal: {
                auto result = [&]() {
                    if (lhsTag == TypeTags::NumberDouble) {
                        if (isNaN(lhsTag, lhsValue) || isNaN(rhsTag, rhsValue)) {
                            return false;
                        }
                        return op(compareDoubleToDecimal(bitcastTo<double>(lhsValue),
                                                         bitcastTo<Decimal128>(rhsValue)),
                                  0);
                    } else if (rhsTag == TypeTags::NumberDouble) {
                        if (isNaN(lhsTag, lhsValue) || isNaN(rhsTag, rhsValue)) {
                            return false;
                        }
                        return op(compareDecimalToDouble(bitcastTo<Decimal128>(lhsValue),
                                                         bitcastTo<double>(rhsValue)),
                                  0);
                    } else {
                        return op(numericCast<Decimal128>(lhsTag, lhsValue),
                                  numericCast<Decimal128>(rhsTag, rhsValue));
                    }
                }();
                return {TypeTags::Boolean, bitcastFrom<bool>(result)};
            }
            default:
                MONGO_UNREACHABLE_TASSERT(11122913);
        }
    } else if (isStringOrSymbol(lhsTag) && isStringOrSymbol(rhsTag)) {
        auto lhsStr = getStringOrSymbolView(lhsTag, lhsValue);
        auto rhsStr = getStringOrSymbolView(rhsTag, rhsValue);
        auto result =
            op(comparator ? comparator->compare(lhsStr, rhsStr) : lhsStr.compare(rhsStr), 0);

        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::Date && rhsTag == TypeTags::Date) {
        auto result = op(bitcastTo<int64_t>(lhsValue), bitcastTo<int64_t>(rhsValue));
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::Timestamp && rhsTag == TypeTags::Timestamp) {
        auto result = op(bitcastTo<uint64_t>(lhsValue), bitcastTo<uint64_t>(rhsValue));
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::Boolean && rhsTag == TypeTags::Boolean) {
        auto result = op(bitcastTo<bool>(lhsValue), bitcastTo<bool>(rhsValue));
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::Null && rhsTag == TypeTags::Null) {
        // This is where Mongo differs from SQL.
        auto result = op(0, 0);
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::MinKey && rhsTag == TypeTags::MinKey) {
        auto result = op(0, 0);
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::MaxKey && rhsTag == TypeTags::MaxKey) {
        auto result = op(0, 0);
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if (lhsTag == TypeTags::bsonUndefined && rhsTag == TypeTags::bsonUndefined) {
        auto result = op(0, 0);
        return {TypeTags::Boolean, bitcastFrom<bool>(result)};
    } else if ((isArray(lhsTag) && isArray(rhsTag)) || (isObject(lhsTag) && isObject(rhsTag)) ||
               (isBinData(lhsTag) && isBinData(rhsTag))) {
        auto [tag, val] = compareValue(lhsTag, lhsValue, rhsTag, rhsValue, comparator);
        if (tag == TypeTags::NumberInt32) {
            auto result = op(bitcastTo<int32_t>(val), 0);
            return {TypeTags::Boolean, bitcastFrom<bool>(result)};
        }
    } else if (isObjectId(lhsTag) && isObjectId(rhsTag)) {
        auto lhsObjId = lhsTag == TypeTags::ObjectId ? getObjectIdView(lhsValue)->data()
                                                     : bitcastTo<uint8_t*>(lhsValue);
        auto rhsObjId = rhsTag == TypeTags::ObjectId ? getObjectIdView(rhsValue)->data()
                                                     : bitcastTo<uint8_t*>(rhsValue);
        auto threeWayResult = memcmp(lhsObjId, rhsObjId, sizeof(ObjectIdType));
        return {TypeTags::Boolean, bitcastFrom<bool>(op(threeWayResult, 0))};
    } else if (lhsTag == TypeTags::bsonRegex && rhsTag == TypeTags::bsonRegex) {
        auto lhsRegex = getBsonRegexView(lhsValue);
        auto rhsRegex = getBsonRegexView(rhsValue);

        if (auto threeWayResult = lhsRegex.pattern.compare(rhsRegex.pattern); threeWayResult != 0) {
            return {TypeTags::Boolean, bitcastFrom<bool>(op(threeWayResult, 0))};
        }

        auto threeWayResult = lhsRegex.flags.compare(rhsRegex.flags);
        return {TypeTags::Boolean, bitcastFrom<bool>(op(threeWayResult, 0))};
    } else if (lhsTag == TypeTags::bsonDBPointer && rhsTag == TypeTags::bsonDBPointer) {
        auto lhsDBPtr = getBsonDBPointerView(lhsValue);
        auto rhsDBPtr = getBsonDBPointerView(rhsValue);
        if (lhsDBPtr.ns.size() != rhsDBPtr.ns.size()) {
            return {TypeTags::Boolean,
                    bitcastFrom<bool>(op(lhsDBPtr.ns.size(), rhsDBPtr.ns.size()))};
        }

        if (auto threeWayResult = lhsDBPtr.ns.compare(rhsDBPtr.ns); threeWayResult != 0) {
            return {TypeTags::Boolean, bitcastFrom<bool>(op(threeWayResult, 0))};
        }

        auto threeWayResult = memcmp(lhsDBPtr.id, rhsDBPtr.id, sizeof(ObjectIdType));
        return {TypeTags::Boolean, bitcastFrom<bool>(op(threeWayResult, 0))};
    } else if (lhsTag == TypeTags::bsonJavascript && rhsTag == TypeTags::bsonJavascript) {
        auto lhsCode = getBsonJavascriptView(lhsValue);
        auto rhsCode = getBsonJavascriptView(rhsValue);
        return {TypeTags::Boolean, bitcastFrom<bool>(op(lhsCode.compare(rhsCode), 0))};
    } else if (lhsTag == TypeTags::bsonCodeWScope && rhsTag == TypeTags::bsonCodeWScope) {
        auto lhsCws = getBsonCodeWScopeView(lhsValue);
        auto rhsCws = getBsonCodeWScopeView(rhsValue);
        if (auto threeWayResult = lhsCws.code.compare(rhsCws.code); threeWayResult != 0) {
            return {TypeTags::Boolean, bitcastFrom<bool>(op(threeWayResult, 0))};
        }

        // Special string comparison semantics do not apply to strings nested inside the
        // CodeWScope scope object, so we do not pass through the string comparator.
        auto [tag, val] = compareValue(TypeTags::bsonObject,
                                       bitcastFrom<const char*>(lhsCws.scope),
                                       TypeTags::bsonObject,
                                       bitcastFrom<const char*>(rhsCws.scope));
        if (tag == TypeTags::NumberInt32) {
            auto result = op(bitcastTo<int32_t>(val), 0);
            return {TypeTags::Boolean, bitcastFrom<bool>(result)};
        }
    }

    return {TypeTags::Nothing, 0};
}
}  // namespace value
}  // namespace sbe
}  // namespace mongo
