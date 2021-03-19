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

#include "mongo/db/exec/sbe/vm/vm.h"

#include "mongo/db/exec/sbe/vm/datetime.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/time_support.h"

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
}  // namespace

/**
 * This is a simple dayOf operation templated by the Op parameter.
 */
template <typename Op>
std::tuple<bool, value::TypeTags, value::Value> genericDayOfOp(value::TypeTags timezoneDBTag,
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

    return {false, value::TypeTags::NumberInt32, value::bitcastTo<int32_t>(result)};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfYear(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDayOfOp<DayOfYear>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfMonth(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDayOfOp<DayOfMonth>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericDayOfWeek(
    value::TypeTags timezoneDBTag,
    value::Value timezoneDBValue,
    value::TypeTags dateTag,
    value::Value dateValue,
    value::TypeTags timezoneTag,
    value::Value timezoneValue) {
    return genericDayOfOp<DayOfWeek>(
        timezoneDBTag, timezoneDBValue, dateTag, dateValue, timezoneTag, timezoneValue);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
