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
            MONGO_UNREACHABLE_TASSERT(11122948);
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
value::TagValueMaybeOwned genericDateExpressionAcceptingTimeZone(value::TagValueView tzDB,
                                                                 value::TagValueView date,
                                                                 value::TagValueView tz) {
    // Get date.
    if (date.tag != value::TypeTags::Date && date.tag != value::TypeTags::Timestamp &&
        date.tag != value::TypeTags::ObjectId && date.tag != value::TypeTags::bsonObjectId) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto dateMs = getDate(date.tag, date.value);

    if (tzDB.tag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(tzDB.value);

    // Get timezone.
    if (!value::isString(tz.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = getTimezone(tz.tag, tz.value, timezoneDB);

    int32_t result;
    Op::doOperation(dateMs, timezone, result);

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
value::TagValueMaybeOwned genericDateExpressionAcceptingTimeZone(value::TagValueView date,
                                                                 value::TagValueView tz) {
    // Get date.
    if (date.tag != value::TypeTags::Date && date.tag != value::TypeTags::Timestamp &&
        date.tag != value::TypeTags::ObjectId && date.tag != value::TypeTags::bsonObjectId) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto dateMs = getDate(date.tag, date.value);

    if (!value::isTimeZone(tz.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = *value::getTimeZoneView(tz.value);

    int32_t result;
    Op::doOperation(dateMs, timezone, result);

    if constexpr (std::is_same<Op, ISOWeekYear>::value) {
        // convert type to long to be compatible with classic
        return {false,
                value::TypeTags::NumberInt64,
                value::bitcastFrom<int64_t>(static_cast<int64_t>(result))};
    } else {
        return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
    }
}

value::TagValueMaybeOwned ByteCode::genericDayOfYear(value::TagValueView tzDB,
                                                     value::TagValueView date,
                                                     value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<DayOfYear>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericDayOfYear(value::TagValueView date,
                                                     value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<DayOfYear>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericDayOfMonth(value::TagValueView tzDB,
                                                      value::TagValueView date,
                                                      value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<DayOfMonth>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericDayOfMonth(value::TagValueView date,
                                                      value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<DayOfMonth>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericDayOfWeek(value::TagValueView tzDB,
                                                     value::TagValueView date,
                                                     value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<DayOfWeek>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericDayOfWeek(value::TagValueView date,
                                                     value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<DayOfWeek>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericYear(value::TagValueView tzDB,
                                                value::TagValueView date,
                                                value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Year>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericYear(value::TagValueView date, value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Year>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericMonth(value::TagValueView tzDB,
                                                 value::TagValueView date,
                                                 value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Month>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericMonth(value::TagValueView date, value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Month>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericHour(value::TagValueView tzDB,
                                                value::TagValueView date,
                                                value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Hour>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericHour(value::TagValueView date, value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Hour>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericMinute(value::TagValueView tzDB,
                                                  value::TagValueView date,
                                                  value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Minute>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericMinute(value::TagValueView date,
                                                  value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Minute>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericSecond(value::TagValueView tzDB,
                                                  value::TagValueView date,
                                                  value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Second>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericSecond(value::TagValueView date,
                                                  value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Second>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericMillisecond(value::TagValueView tzDB,
                                                       value::TagValueView date,
                                                       value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Millisecond>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericMillisecond(value::TagValueView date,
                                                       value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Millisecond>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericWeek(value::TagValueView tzDB,
                                                value::TagValueView date,
                                                value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Week>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericWeek(value::TagValueView date, value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<Week>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericISOWeekYear(value::TagValueView tzDB,
                                                       value::TagValueView date,
                                                       value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<ISOWeekYear>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericISOWeekYear(value::TagValueView date,
                                                       value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<ISOWeekYear>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericISODayOfWeek(value::TagValueView tzDB,
                                                        value::TagValueView date,
                                                        value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<ISODayOfWeek>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericISODayOfWeek(value::TagValueView date,
                                                        value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<ISODayOfWeek>(date, tz);
}

value::TagValueMaybeOwned ByteCode::genericISOWeek(value::TagValueView tzDB,
                                                   value::TagValueView date,
                                                   value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<ISOWeek>(tzDB, date, tz);
}

value::TagValueMaybeOwned ByteCode::genericISOWeek(value::TagValueView date,
                                                   value::TagValueView tz) {
    return genericDateExpressionAcceptingTimeZone<ISOWeek>(date, tz);
}
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
