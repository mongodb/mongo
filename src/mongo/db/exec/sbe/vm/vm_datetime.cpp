/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/vm/vm_datetime.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cstdint>

namespace mongo {
namespace sbe {
namespace vm {

bool isValidTimezone(value::TypeTags timezoneTag,
                     value::Value timezoneValue,
                     const TimeZoneDatabase* timezoneDB) {
    if (!value::isString(timezoneTag)) {
        return false;
    }
    auto timezoneStringView = value::getStringView(timezoneTag, timezoneValue);
    return timezoneStringView.empty() || timezoneDB->isTimeZoneIdentifier(timezoneStringView);
}

TimeZone getTimezone(value::TypeTags timezoneTag,
                     value::Value timezoneVal,
                     TimeZoneDatabase* timezoneDB) {
    auto timezoneStr = value::getStringView(timezoneTag, timezoneVal);
    if (timezoneStr.empty()) {
        return timezoneDB->utcZone();
    } else {
        return timezoneDB->getTimeZone(timezoneStr);
    }
}

Date_t getDate(value::TypeTags dateTag, value::Value dateVal) {
    switch (dateTag) {
        case value::TypeTags::Date: {
            return Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(dateVal));
        }
        case value::TypeTags::Timestamp: {
            return Date_t::fromMillisSinceEpoch(
                Timestamp(value::bitcastTo<uint64_t>(dateVal)).getSecs() * 1000LL);
        }
        case value::TypeTags::ObjectId: {
            auto objIdBuf = value::getObjectIdView(dateVal);
            auto objId = OID::from(objIdBuf);
            return objId.asDateT();
        }
        case value::TypeTags::bsonObjectId: {
            auto objIdBuf = value::getRawPointerView(dateVal);
            auto objId = OID::from(objIdBuf);
            return objId.asDateT();
        }
        default:
            MONGO_UNREACHABLE;
    }
}

bool coercibleToDate(value::TypeTags typeTag) {
    return typeTag == value::TypeTags::Date || typeTag == value::TypeTags::Timestamp ||
        typeTag == value::TypeTags::ObjectId || typeTag == value::TypeTags::bsonObjectId;
}

namespace {

/**
 * The dayOfYear operation used by genericDayOfOp.
 */
struct DayOfYear {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dayOfYear(date);
    }
};

/**
 * The dayOfMonth operation used by genericDayOfOp.
 */
struct DayOfMonth {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dayOfMonth(date);
    }
};

/**
 * The dayOfWeek operation used by genericDayOfOp.
 */
struct DayOfWeek {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dayOfWeek(date);
    }
};

/**
 * The year operation used by genericExtractFromDate.
 */
struct Year {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dateParts(date).year;
    }
};

/**
 * The month operation used by genericExtractFromDate.
 */
struct Month {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dateParts(date).month;
    }
};

/**
 * The hour operation used by genericExtractFromDate.
 */
struct Hour {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dateParts(date).hour;
    }
};

/**
 * The minute operation used by genericExtractFromDate.
 */
struct Minute {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dateParts(date).minute;
    }
};

/**
 * The second operation used by genericExtractFromDate.
 */
struct Second {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dateParts(date).second;
    }
};

/**
 * The millisecond operation used by genericExtractFromDate.
 */
struct Millisecond {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.dateParts(date).millisecond;
    }
};

/**
 * The week operation used by genericExtractFromDate.
 */
struct Week {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.week(date);
    }
};

/**
 * The ISOWeekYear operation used by genericExtractFromDate.
 */
struct ISOWeekYear {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.isoYear(date);
    }
};

/**
 * The ISODayOfWeek operation used by genericExtractFromDate.
 */
struct ISODayOfWeek {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.isoDayOfWeek(date);
    }
};

/**
 * The ISOWeek operation used by genericExtractFromDate.
 */
struct ISOWeek {
    static void doOperation(const Date_t& date, const TimeZone& timezone, int32_t& result) {
        result = timezone.isoWeek(date);
    }
};
}  // namespace

/**
 * This is a simple date expression operation templated by the Op parameter which takes
 * timezone string as argument
 */
template <typename Op>
FastTuple<bool, value::TypeTags, value::Value> genericDateExpressionAcceptingTimeZone(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    // Get date.
    if (dateTag != value::TypeTags::Date && dateTag != value::TypeTags::Timestamp &&
        dateTag != value::TypeTags::ObjectId && dateTag != value::TypeTags::bsonObjectId) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto date = getDate(dateTag, dateValue);

    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBValue);

    // Get timezone.
    if (!value::isString(timezoneTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = getTimezone(timezoneTag, timezoneValue, timezoneDB);

    int32_t result;
    Op::doOperation(date, timezone, result);

    if constexpr (std::is_same<Op, ISOWeekYear>::value) {
        // convert type to long to be compatible with classic
        return {false,
                value::TypeTags::NumberInt64,
                value::bitcastFrom<int64_t>(static_cast<int64_t>(result))};
    } else {
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
    }
}

/**
 * This is a simple date expression operation templated by the Op parameter which takes
 * timezone object as argument
 */
template <typename Op>
FastTuple<bool, value::TypeTags, value::Value> genericDateExpressionAcceptingTimeZone(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    // Get date.
    if (dateTag != value::TypeTags::Date && dateTag != value::TypeTags::Timestamp &&
        dateTag != value::TypeTags::ObjectId && dateTag != value::TypeTags::bsonObjectId) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto date = getDate(dateTag, dateValue);

    if (!value::isTimeZone(timezoneTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = *value::getTimeZoneView(timezoneValue);

    int32_t result;
    Op::doOperation(date, timezone, result);

    if constexpr (std::is_same<Op, ISOWeekYear>::value) {
        // convert type to long to be compatible with classic
        return {false,
                value::TypeTags::NumberInt64,
                value::bitcastFrom<int64_t>(static_cast<int64_t>(result))};
    } else {
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfYear(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<DayOfYear>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfYear(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<DayOfYear>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfMonth(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<DayOfMonth>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfMonth(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<DayOfMonth>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfWeek(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<DayOfWeek>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfWeek(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<DayOfWeek>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericYear(value::TypeTags timezoneDBTag,
                                                                     value::Value timezoneDBValue,
                                                                     value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Year>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericYear(value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Year>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMonth(value::TypeTags timezoneDBTag,
                                                                      value::Value timezoneDBValue,
                                                                      value::TypeTags dateTag,
                                                                      value::Value dateValue,
                                                                      value::TypeTags timezoneTag,
                                                                      value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Month>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMonth(value::TypeTags dateTag,
                                                                      value::Value dateValue,
                                                                      value::TypeTags timezoneTag,
                                                                      value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Month>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericHour(value::TypeTags timezoneDBTag,
                                                                     value::Value timezoneDBValue,
                                                                     value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Hour>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericHour(value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Hour>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMinute(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Minute>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMinute(value::TypeTags dateTag,
                                                                       value::Value dateValue,
                                                                       value::TypeTags timezoneTag,
                                                                       value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Minute>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericSecond(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Second>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericSecond(value::TypeTags dateTag,
                                                                       value::Value dateValue,
                                                                       value::TypeTags timezoneTag,
                                                                       value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Second>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMillisecond(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Millisecond>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMillisecond(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Millisecond>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericWeek(value::TypeTags timezoneDBTag,
                                                                     value::Value timezoneDBValue,
                                                                     value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Week>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericWeek(value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<Week>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericISOWeekYear(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<ISOWeekYear>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericISOWeekYear(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<ISOWeekYear>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericISODayOfWeek(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<ISODayOfWeek>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericISODayOfWeek(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<ISODayOfWeek>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericISOWeek(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<ISOWeek>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericISOWeek(
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDateExpressionAcceptingTimeZone<ISOWeek>(
        dateTag, dateValue, timezoneTag, timezoneValue);
}
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
