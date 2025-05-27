/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/compare_numbers.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"

#include <cmath>

#include <boost/cstdint.hpp>
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
                MONGO_UNREACHABLE;
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
