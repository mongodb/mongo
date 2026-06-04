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

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_datetime.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe::vm {
using ColumnOpType = value::ColumnOpType;

MONGO_FAIL_POINT_DEFINE(sleepBeforeCurrentDateEvaluationSBE);

namespace {
const size_t kTimezoneDBStackPosDefault = 0u;
const size_t kTimezoneDBStackPosBlock = 2u;
const size_t kStackPosOffsetBlock = 1u;
}  // namespace

/**
 * A helper for the builtinDate method. The formal parameters yearOrWeekYear and monthOrWeek carry
 * values depending on wether the date is a year-month-day or ISOWeekYear.
 */
using DateFn = std::function<Date_t(
    TimeZone, long long, long long, long long, long long, long long, long long, long long)>;
value::TagValueMaybeOwned builtinDateHelper(DateFn computeDateFn,
                                            value::TagValueView tzdb,
                                            value::TagValueView yearOrWeekYear,
                                            value::TagValueView monthOrWeek,
                                            value::TagValueView day,
                                            value::TagValueView hour,
                                            value::TagValueView minute,
                                            value::TagValueView second,
                                            value::TagValueView millisecond,
                                            value::TagValueView timezone) {

    if (tzdb.tag != value::TypeTags::timeZoneDB || !value::isNumber(yearOrWeekYear.tag) ||
        !value::isNumber(monthOrWeek.tag) || !value::isNumber(day.tag) ||
        !value::isNumber(hour.tag) || !value::isNumber(minute.tag) ||
        !value::isNumber(second.tag) || !value::isNumber(millisecond.tag) ||
        !value::isString(timezone.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    tassert(11054000, "unexpected TZDB value", tzdb.value);
    auto timeZoneDB = value::getTimeZoneDBView(tzdb.value);

    auto tzString = value::getStringView(timezone.tag, timezone.value);
    const auto tz = tzString == "" ? timeZoneDB->utcZone() : timeZoneDB->getTimeZone(tzString);

    auto date = computeDateFn(tz,
                              value::numericCast<int64_t>(yearOrWeekYear),
                              value::numericCast<int64_t>(monthOrWeek),
                              value::numericCast<int64_t>(day),
                              value::numericCast<int64_t>(hour),
                              value::numericCast<int64_t>(minute),
                              value::numericCast<int64_t>(second),
                              value::numericCast<int64_t>(millisecond));
    return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(date.asInt64())};
}

value::TagValueMaybeOwned ByteCode::builtinDate(ArityType arity) {
    return builtinDateHelper(
        [](TimeZone tz,
           long long year,
           long long month,
           long long day,
           long long hour,
           long long min,
           long long sec,
           long long millis) -> Date_t {
            return tz.createFromDateParts(year, month, day, hour, min, sec, millis);
        },
        viewFromStack(0),
        viewFromStack(1),
        viewFromStack(2),
        viewFromStack(3),
        viewFromStack(4),
        viewFromStack(5),
        viewFromStack(6),
        viewFromStack(7),
        viewFromStack(8));
}

value::TagValueMaybeOwned ByteCode::builtinDateToString(ArityType arity) {
    tassert(11080051, "Unexpected arity value", arity == 4);

    auto timezoneDBView = viewFromStack(0);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);

    // Get date.
    auto dateView = viewFromStack(1);
    if (!coercibleToDate(dateView.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto date = getDate(dateView);

    // Get format.
    auto formatView = viewFromStack(2);
    if (!value::isString(formatView.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto formatString = value::getStringView(formatView.tag, formatView.value);
    if (!TimeZone::isValidToStringFormat(formatString)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Get timezone.
    auto timezoneView = viewFromStack(3);
    if (!isValidTimezone(timezoneView, timezoneDB)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezone = getTimezone(timezoneView, timezoneDB);

    StringBuilder formatted;

    auto status = timezone.outputDateWithFormat(formatted, formatString, date);

    if (status != Status::OK()) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [strTag, strValue] = sbe::value::makeNewString(formatted.stringData());
    return {true, strTag, strValue};
}

value::TagValueMaybeOwned ByteCode::builtinDateFromString(ArityType arity) {
    auto timezoneDBView = viewFromStack(0);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);

    // Get parameter tuples from stack.
    auto dateStringView = viewFromStack(1);
    auto timezoneView = viewFromStack(2);

    auto timezone = getTimezone(timezoneView, timezoneDB);

    // Attempt to get the date from the string. This may throw a ConversionFailure error.
    Date_t date;
    auto dateString = value::getStringView(dateStringView.tag, dateStringView.value);
    if (arity == 3) {
        // Format wasn't specified, so we call fromString without it.
        date = timezoneDB->fromString(dateString, timezone);
    } else {
        // Fetch format from the stack, validate it, and call fromString with it.
        auto formatView = viewFromStack(3);
        if (!value::isString(formatView.tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        auto formatString = value::getStringView(formatView.tag, formatView.value);
        if (!TimeZone::isValidFromStringFormat(formatString)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        date = timezoneDB->fromString(dateString, timezone, formatString);
    }

    return {true, value::TypeTags::Date, value::bitcastFrom<int64_t>(date.toMillisSinceEpoch())};
}

value::TagValueMaybeOwned ByteCode::builtinDateFromStringNoThrow(ArityType arity) {
    try {
        return builtinDateFromString(arity);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        // Upon error, we return Nothing and let the caller decide whether to raise an error.
        return {false, value::TypeTags::Nothing, 0};
    }
}

value::TagValueMaybeOwned ByteCode::dateTrunc(value::TypeTags dateTag,
                                              value::Value dateValue,
                                              TimeUnit unit,
                                              int64_t binSize,
                                              TimeZone timezone,
                                              DayOfWeek startOfWeek) {
    // Get date.
    if (!coercibleToDate(dateTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto date = getDate({dateTag, dateValue});

    auto truncatedDate = truncateDate(date, unit, binSize, timezone, startOfWeek);
    return {false,
            value::TypeTags::Date,
            value::bitcastFrom<int64_t>(truncatedDate.toMillisSinceEpoch())};
}

value::TagValueMaybeOwned ByteCode::builtinDateWeekYear(ArityType arity) {
    return builtinDateHelper(
        [](TimeZone tz,
           long long year,
           long long month,
           long long day,
           long long hour,
           long long min,
           long long sec,
           long long millis) -> Date_t {
            return tz.createFromIso8601DateParts(year, month, day, hour, min, sec, millis);
        },
        viewFromStack(0),
        viewFromStack(1),
        viewFromStack(2),
        viewFromStack(3),
        viewFromStack(4),
        viewFromStack(5),
        viewFromStack(6),
        viewFromStack(7),
        viewFromStack(8));
}

value::TagValueMaybeOwned ByteCode::builtinDateToParts(ArityType arity) {
    auto timezoneDBView = viewFromStack(0);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);
    auto dateView = viewFromStack(1);

    // Get timezone.
    auto timezoneView = viewFromStack(2);
    if (!value::isString(timezoneView.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    TimeZone timezone = getTimezone(timezoneView, timezoneDB);

    // Get date.
    if (!value::tagIn(dateView.tag,
                      value::TypeTags::Date,
                      value::TypeTags::Timestamp,
                      value::TypeTags::ObjectId,
                      value::TypeTags::bsonObjectId)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    Date_t date = getDate(dateView);

    // Get date parts.
    auto dateParts = timezone.dateParts(date);
    auto [dateObjTag, dateObjVal] = value::makeNewObject();
    value::ValueGuard guard{dateObjTag, dateObjVal};
    auto dateObj = value::getObjectView(dateObjVal);
    dateObj->reserve(7);
    dateObj->push_back_raw("year", value::TypeTags::NumberInt32, dateParts.year);
    dateObj->push_back_raw("month", value::TypeTags::NumberInt32, dateParts.month);
    dateObj->push_back_raw("day", value::TypeTags::NumberInt32, dateParts.dayOfMonth);
    dateObj->push_back_raw("hour", value::TypeTags::NumberInt32, dateParts.hour);
    dateObj->push_back_raw("minute", value::TypeTags::NumberInt32, dateParts.minute);
    dateObj->push_back_raw("second", value::TypeTags::NumberInt32, dateParts.second);
    dateObj->push_back_raw("millisecond", value::TypeTags::NumberInt32, dateParts.millisecond);
    guard.reset();
    return {true, dateObjTag, dateObjVal};
}

value::TagValueMaybeOwned ByteCode::builtinIsoDateToParts(ArityType arity) {
    auto timezoneDBView = viewFromStack(0);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);
    auto dateView = viewFromStack(1);

    // Get timezone.
    auto timezoneView = viewFromStack(2);
    if (!value::isString(timezoneView.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    TimeZone timezone = getTimezone(timezoneView, timezoneDB);

    // Get date.
    if (!value::tagIn(dateView.tag,
                      value::TypeTags::Date,
                      value::TypeTags::Timestamp,
                      value::TypeTags::ObjectId,
                      value::TypeTags::bsonObjectId)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    Date_t date = getDate(dateView);

    // Get date parts.
    auto dateParts = timezone.dateIso8601Parts(date);
    auto [dateObjTag, dateObjVal] = value::makeNewObject();
    value::ValueGuard guard{dateObjTag, dateObjVal};
    auto dateObj = value::getObjectView(dateObjVal);
    dateObj->reserve(7);
    dateObj->push_back_raw("isoWeekYear", value::TypeTags::NumberInt32, dateParts.year);
    dateObj->push_back_raw("isoWeek", value::TypeTags::NumberInt32, dateParts.weekOfYear);
    dateObj->push_back_raw("isoDayOfWeek", value::TypeTags::NumberInt32, dateParts.dayOfWeek);
    dateObj->push_back_raw("hour", value::TypeTags::NumberInt32, dateParts.hour);
    dateObj->push_back_raw("minute", value::TypeTags::NumberInt32, dateParts.minute);
    dateObj->push_back_raw("second", value::TypeTags::NumberInt32, dateParts.second);
    dateObj->push_back_raw("millisecond", value::TypeTags::NumberInt32, dateParts.millisecond);
    guard.reset();
    return {true, dateObjTag, dateObjVal};
}

value::TagValueMaybeOwned ByteCode::builtinDayOfYear(ArityType arity) {
    tassert(11080050, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericDayOfYear(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericDayOfYear(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinDayOfMonth(ArityType arity) {
    tassert(11080049, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericDayOfMonth(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericDayOfMonth(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinDayOfWeek(ArityType arity) {
    tassert(11080048, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericDayOfWeek(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericDayOfWeek(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinYear(ArityType arity) {
    tassert(11080047, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericYear(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericYear(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinMonth(ArityType arity) {
    tassert(11080046, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericMonth(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericMonth(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinHour(ArityType arity) {
    tassert(11080045, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericHour(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericHour(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinMinute(ArityType arity) {
    tassert(11080044, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericMinute(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericMinute(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinSecond(ArityType arity) {
    tassert(11080043, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericSecond(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericSecond(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinMillisecond(ArityType arity) {
    tassert(11080042, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericMillisecond(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericMillisecond(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinWeek(ArityType arity) {
    tassert(11080041, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericWeek(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericWeek(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinISOWeekYear(ArityType arity) {
    tassert(11080040, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericISOWeekYear(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericISOWeekYear(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinISODayOfWeek(ArityType arity) {
    tassert(11080039, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericISODayOfWeek(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericISODayOfWeek(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinISOWeek(ArityType arity) {
    tassert(11080038, "Unexpected arity value", arity == 3 || arity == 2);

    auto date = viewFromStack(0);
    if (arity == 3) {
        auto tzDB = viewFromStack(1);
        auto tz = viewFromStack(2);
        return genericISOWeek(tzDB, date, tz);
    } else {
        auto tz = viewFromStack(1);
        return genericISOWeek(date, tz);
    }
}

value::TagValueMaybeOwned ByteCode::builtinIsTimeUnit(ArityType arity) {
    tassert(11080037, "Unexpected arity value", arity == 1);
    auto timeUnit = viewFromStack(0);
    if (!value::isString(timeUnit.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                isValidTimeUnit(value::getStringView(timeUnit.tag, timeUnit.value)))};
}

value::TagValueMaybeOwned ByteCode::builtinIsDayOfWeek(ArityType arity) {
    tassert(11080036, "Unexpected arity value", arity == 1);
    auto dayOfWeek = viewFromStack(0);
    if (!value::isString(dayOfWeek.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                isValidDayOfWeek(value::getStringView(dayOfWeek.tag, dayOfWeek.value)))};
}

value::TagValueMaybeOwned ByteCode::builtinIsTimezone(ArityType arity) {
    auto tzDB = viewFromStack(0);
    if (tzDB.tag != value::TypeTags::timeZoneDB) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto timezoneDB = value::getTimeZoneDBView(tzDB.value);
    auto tz = viewFromStack(1);
    if (!value::isString(tz.tag)) {
        return {false, value::TypeTags::Boolean, false};
    }
    auto timezoneStr = value::getStringView(tz.tag, tz.value);
    if (timezoneDB->isTimeZoneIdentifier(timezoneStr)) {
        return {false, value::TypeTags::Boolean, true};
    }
    return {false, value::TypeTags::Boolean, false};
}

value::TagValueMaybeOwned ByteCode::builtinTsSecond(ArityType arity) {
    tassert(11080035, "Unexpected arity value", arity == 1);

    auto input = viewFromStack(0);

    if (input.tag != value::TypeTags::Timestamp) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto timestamp = Timestamp(value::bitcastTo<uint64_t>(input.value));
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<uint64_t>(timestamp.getSecs())};
}

value::TagValueMaybeOwned ByteCode::builtinTsIncrement(ArityType arity) {
    tassert(11080034, "Unexpected arity value", arity == 1);

    auto input = viewFromStack(0);

    if (input.tag != value::TypeTags::Timestamp) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto timestamp = Timestamp(value::bitcastTo<uint64_t>(input.value));
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<uint64_t>(timestamp.getInc())};
}

/**
 * The stack for builtinDateTrunc is ordered as follows:
 * (0) timezoneDB
 * (1) date
 * (2) timeUnit
 * ...
 *
 * The stack for builtinValueBlockDateTrunc is ordered as follows:
 * (0) bitset
 * (1) dateBlock
 * (2) timezoneDB
 * (3) timeUnit
 *
 * This difference in stack positions is handled by the isBlockBuiltin parameter.
 */
template <bool IsBlockBuiltin>
bool ByteCode::validateDateTruncParameters(TimeUnit* unit,
                                           int64_t* binSize,
                                           TimeZone* timezone,
                                           DayOfWeek* startOfWeek) {
    size_t timezoneDBStackPos =
        IsBlockBuiltin ? kTimezoneDBStackPosBlock : kTimezoneDBStackPosDefault;
    auto timezoneDBView = viewFromStack(timezoneDBStackPos);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return false;
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);

    size_t stackPosOffset = IsBlockBuiltin ? kStackPosOffsetBlock : 0u;

    auto unitView = viewFromStack(2 + stackPosOffset);
    if (!value::isString(unitView.tag)) {
        return false;
    }
    auto unitString = value::getStringView(unitView.tag, unitView.value);
    if (!isValidTimeUnit(unitString)) {
        return false;
    }
    *unit = parseTimeUnit(unitString);

    // Get binSize.
    auto binSizeView = viewFromStack(3 + stackPosOffset);
    if (!value::isNumber(binSizeView.tag)) {
        return false;
    }
    auto binSizeLong =
        value::genericNumConvert(binSizeView.tag, binSizeView.value, value::TypeTags::NumberInt64);
    if (binSizeLong.tag() == value::TypeTags::Nothing) {
        return false;
    }
    *binSize = value::bitcastTo<int64_t>(binSizeLong.value());
    if (*binSize <= 0) {
        return false;
    }

    // Get timezone.
    auto timezoneView = viewFromStack(4 + stackPosOffset);
    if (!isValidTimezone(timezoneView, timezoneDB)) {
        return false;
    }
    *timezone = getTimezone(timezoneView, timezoneDB);

    // Get startOfWeek, if 'startOfWeek' parameter was passed and time unit is the week.
    if (*unit == TimeUnit::week) {
        auto startOfWeekView = viewFromStack(5 + stackPosOffset);
        if (!value::isString(startOfWeekView.tag)) {
            return false;
        }
        auto startOfWeekString = value::getStringView(startOfWeekView.tag, startOfWeekView.value);
        if (!isValidDayOfWeek(startOfWeekString)) {
            return false;
        }
        *startOfWeek = parseDayOfWeek(startOfWeekString);
    }

    return true;
}

value::TagValueMaybeOwned ByteCode::builtinDateTrunc(ArityType arity) {
    tassert(11080033, "Unexpected arity value", arity == 6);

    TimeUnit unit{TimeUnit::year};
    int64_t binSize{0u};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateTruncParameters<>(&unit, &binSize, &timezone, &startOfWeek)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Get date.
    auto dateView = viewFromStack(1);

    return dateTrunc(dateView.tag, dateView.value, unit, binSize, timezone, startOfWeek);
}


/**
 * The stack for builtinDateDiff is ordered as follows:
 * (0) timezoneDB
 * (1) start date
 * (2) end date
 * (3) timeUnit
 * (4) timezone
 * (5) optional start of week
 *
 * The stack for builtinValueBlockDateDiff is ordered as follows:
 * (0) bitset
 * (1) start dateBlock
 * (2) timezoneDB
 * (3) end date
 * (4) timeUnit
 * (5) timezone
 * (6) optional start of week
 *
 * This difference in stack positions is handled by the isBlockBuiltin parameter.
 */
template <bool IsBlockBuiltin>
bool ByteCode::validateDateDiffParameters(Date_t* endDate,
                                          TimeUnit* unit,
                                          TimeZone* timezone,
                                          DayOfWeek* startOfWeek) {
    size_t timezoneDBStackPos =
        IsBlockBuiltin ? kTimezoneDBStackPosBlock : kTimezoneDBStackPosDefault;
    auto timezoneDBView = viewFromStack(timezoneDBStackPos);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return false;
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);

    size_t stackPosOffset = IsBlockBuiltin ? kStackPosOffsetBlock : 0u;

    auto endDateView = viewFromStack(2 + stackPosOffset);
    if (!coercibleToDate(endDateView.tag)) {
        return false;
    }
    *endDate = getDate(endDateView);

    auto unitView = viewFromStack(3 + stackPosOffset);
    if (!value::isString(unitView.tag)) {
        return false;
    }
    auto unitString = value::getStringView(unitView.tag, unitView.value);
    if (!isValidTimeUnit(unitString)) {
        return false;
    }
    *unit = parseTimeUnit(unitString);

    // Get timezone.
    auto timezoneView = viewFromStack(4 + stackPosOffset);
    if (!isValidTimezone(timezoneView, timezoneDB)) {
        return false;
    }
    *timezone = getTimezone(timezoneView, timezoneDB);

    // Get startOfWeek, if 'startOfWeek' parameter was requested and time unit is the week.
    if (startOfWeek) {
        auto startOfWeekView = viewFromStack(5 + stackPosOffset);
        if (!value::isString(startOfWeekView.tag)) {
            return false;
        }
        if (TimeUnit::week == *unit) {
            auto startOfWeekString =
                value::getStringView(startOfWeekView.tag, startOfWeekView.value);
            if (!isValidDayOfWeek(startOfWeekString)) {
                return false;
            }
            *startOfWeek = parseDayOfWeek(startOfWeekString);
        }
    }
    return true;
}

value::TagValueMaybeOwned ByteCode::builtinDateDiff(ArityType arity) {
    tassert(11080032,
            "Unexpected arity value",
            arity == 5 || arity == 6);  // 6th parameter is 'startOfWeek'.

    Date_t endDate;
    TimeUnit unit{TimeUnit::year};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateDiffParameters<>(
            &endDate, &unit, &timezone, arity == 6 ? &startOfWeek : nullptr)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Get startDate.
    auto startDateView = viewFromStack(1);
    if (!coercibleToDate(startDateView.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto startDate = getDate(startDateView);

    auto result = dateDiff(startDate, endDate, unit, timezone, startOfWeek);
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
}


/**
 * The stack for builtinDateAdd is ordered as follows:
 * (0) timezoneDB
 * (1) date
 * (2) timeUnit
 * ...
 *
 * The stack for builtinValueBlockDateAdd is ordered as follows:
 * (0) bitset
 * (1) dateBlock
 * (2) timezoneDB
 * (3) timeUnit
 * ...
 *
 * This difference in stack positions is handled by the isBlockBuiltin parameter.
 */
template <bool IsBlockBuiltin>
bool ByteCode::validateDateAddParameters(TimeUnit* unit, int64_t* amount, TimeZone* timezone) {
    size_t timezoneDBStackPos =
        IsBlockBuiltin ? kTimezoneDBStackPosBlock : kTimezoneDBStackPosDefault;
    auto timezoneDBView = viewFromStack(timezoneDBStackPos);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        return false;
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);

    size_t stackPosOffset = IsBlockBuiltin ? kStackPosOffsetBlock : 0u;

    auto unitView = viewFromStack(2 + stackPosOffset);
    if (!value::isString(unitView.tag)) {
        return false;
    }
    std::string unitStr{value::getStringView(unitView.tag, unitView.value)};
    if (!isValidTimeUnit(unitStr)) {
        return false;
    }
    *unit = parseTimeUnit(unitStr);

    auto amountView = viewFromStack(3 + stackPosOffset);
    if (amountView.tag != value::TypeTags::NumberInt64) {
        return false;
    }
    *amount = value::bitcastTo<int64_t>(amountView.value);

    auto timezoneView = viewFromStack(4 + stackPosOffset);
    if (!value::isString(timezoneView.tag) || !isValidTimezone(timezoneView, timezoneDB)) {
        return false;
    }
    *timezone = getTimezone(timezoneView, timezoneDB);
    return true;
}

value::TagValueMaybeOwned ByteCode::builtinDateAdd(ArityType arity) {
    tassert(11080031, "Unexpected arity value", arity == 5);
    TimeUnit unit{TimeUnit::year};
    int64_t amount;
    TimeZone timezone{};

    if (!validateDateAddParameters<>(&unit, &amount, &timezone)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto startDateView = viewFromStack(1);
    if (!coercibleToDate(startDateView.tag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto startDate = getDate(startDateView);

    auto resDate = dateAdd(startDate, unit, amount, timezone);
    return {
        false, value::TypeTags::Date, value::bitcastFrom<int64_t>(resDate.toMillisSinceEpoch())};
}

namespace {

struct DateTruncFunctor {
    DateTruncFunctor(TimeUnit unit, int64_t binSize, TimeZone timeZone, DayOfWeek startOfWeek)
        : _unit(unit), _binSize(binSize), _timeZone(timeZone), _startOfWeek(startOfWeek) {
        _dateReferencePoint = defaultReferencePointForDateTrunc(timeZone, unit, startOfWeek);
    }

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = getDate({tag, val});

        auto truncatedDate =
            truncateDate(date, _unit, _binSize, _dateReferencePoint, _timeZone, _startOfWeek);

        return std::pair(value::TypeTags::Date,
                         value::bitcastFrom<int64_t>(truncatedDate.toMillisSinceEpoch()));
    }

    TimeUnit _unit;
    int64_t _binSize;
    TimeZone _timeZone;
    DayOfWeek _startOfWeek;
    DateReferencePoint _dateReferencePoint;
};

static const auto dateTruncOp =
    value::makeColumnOpWithParams<ColumnOpType::kMonotonic, DateTruncFunctor>();

struct DateTruncMillisFunctor {
    DateTruncMillisFunctor(TimeUnit unit, int64_t binSize, TimeZone timeZone, DayOfWeek startOfWeek)
        : _binSize(binSize) {
        _binSize = getBinSizeInMillis(binSize, unit);
        _referencePointInMillis =
            defaultReferencePointForDateTrunc(timeZone, unit, startOfWeek).dateMillis;
    }

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = getDate({tag, val});

        auto truncatedDate = truncateDateMillis(date, _referencePointInMillis, _binSize);

        return std::pair(value::TypeTags::Date,
                         value::bitcastFrom<int64_t>(truncatedDate.toMillisSinceEpoch()));
    }

    int64_t _binSize;
    Date_t _referencePointInMillis;
};

static const auto dateTruncMillisOp =
    value::makeColumnOpWithParams<ColumnOpType::kMonotonic, DateTruncMillisFunctor>();

struct DateDiffFunctor {
    DateDiffFunctor(Date_t endDate, TimeUnit unit, TimeZone timeZone, DayOfWeek startOfWeek)
        : _endDate(timeZone.getTimelibTime(endDate)),
          _unit(unit),
          _timeZone(timeZone),
          _startOfWeek(startOfWeek) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = _timeZone.getTimelibTime(getDate({tag, val}));

        auto result = dateDiff(date.get(), _endDate.get(), _unit, _startOfWeek);

        return std::pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result));
    }

    std::unique_ptr<_timelib_time, TimeZone::TimelibTimeDeleter> _endDate;
    TimeUnit _unit;
    TimeZone _timeZone;
    DayOfWeek _startOfWeek;
};

static const auto dateDiffOp =
    value::makeColumnOpWithParams<ColumnOpType::kMonotonic, DateDiffFunctor>();

struct DateDiffMillisecondFunctor {
    DateDiffMillisecondFunctor(Date_t endDate) : _endDate(endDate) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = getDate({tag, val});

        auto result = dateDiffMillisecond(date, _endDate);

        return std::pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result));
    }

    Date_t _endDate;
};

static const auto dateDiffMillisecondOp =
    value::makeColumnOpWithParams<ColumnOpType::kMonotonic, DateDiffMillisecondFunctor>();

struct DateAddFunctor {
    DateAddFunctor(TimeUnit unit, int64_t amount, TimeZone timeZone)
        : _unit(unit), _amount(amount), _timeZone(timeZone) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = getDate({tag, val});

        auto res = dateAdd(date, _unit, _amount, _timeZone);

        return std::pair(value::TypeTags::Date,
                         value::bitcastFrom<int64_t>(res.toMillisSinceEpoch()));
    }

    TimeUnit _unit;
    int64_t _amount;
    TimeZone _timeZone;
};

static const auto dateAddOp =
    value::makeColumnOpWithParams<ColumnOpType::kMonotonic, DateAddFunctor>();
}  // namespace

namespace {
value::TagValueMaybeOwned makeNothingBlock(value::ValueBlock* block) {
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(
                value::MonoBlock::makeNothingBlock(block->count()).release())};
}
}  // namespace

/**
 * Given a ValueBlock and bitset as input, returns a ValueBlock where each date in the input block
 * with corresponding bit set to true have been truncated based on arguments provided. Values that
 * are not coercible to dates are turned into Nothings instead.
 */
value::TagValueMaybeOwned ByteCode::builtinValueBlockDateTrunc(ArityType arity) {
    tassert(11080030, "Unexpected arity value", arity == 7);

    auto inputView = viewFromStack(1);
    tassert(8625725,
            "Expected input argument to be of valueBlock type",
            inputView.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputView.value);

    auto bitsetView = viewFromStack(0);
    // A bitmap argument set to Nothing is equivalent to a bitmap made of all True values.
    tassert(8625726,
            "Expected bitset argument to be of either Nothing or valueBlock type",
            bitsetView.tag == value::TypeTags::Nothing ||
                bitsetView.tag == value::TypeTags::valueBlock);

    TimeUnit unit{TimeUnit::year};
    int64_t binSize{0u};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateTruncParameters<true /* isBlockBuiltin */>(
            &unit, &binSize, &timezone, &startOfWeek)) {
        return makeNothingBlock(valueBlockIn);
    }

    switch (unit) {
        case TimeUnit::millisecond:
        case TimeUnit::second:
        case TimeUnit::minute:
        case TimeUnit::hour: {
            auto out = valueBlockIn->map(
                dateTruncMillisOp.bindParams(unit, binSize, timezone, startOfWeek));
            return {true,
                    value::TypeTags::valueBlock,
                    value::bitcastFrom<value::ValueBlock*>(out.release())};
        }
        default: {
            auto out =
                valueBlockIn->map(dateTruncOp.bindParams(unit, binSize, timezone, startOfWeek));
            return {true,
                    value::TypeTags::valueBlock,
                    value::bitcastFrom<value::ValueBlock*>(out.release())};
        }
    }
}

/**
 * Given a ValueBlock and bitset as input, returns a ValueBlock with the difference between each
 * date in the input block with corresponding bit set to true and the argument provided. Values that
 * are not coercible to dates are turned into Nothings instead.
 */
value::TagValueMaybeOwned ByteCode::builtinValueBlockDateDiff(ArityType arity) {
    tassert(11080029, "Unexpected arity value", arity == 6 || arity == 7);

    auto inputView = viewFromStack(1);
    tassert(8625727,
            "Expected input argument to be of valueBlock type",
            inputView.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputView.value);

    auto bitsetView = viewFromStack(0);
    // A bitmap argument set to Nothing is equivalent to a bitmap made of all True values.
    tassert(8625728,
            "Expected bitset argument to be of either Nothing or valueBlock type",
            bitsetView.tag == value::TypeTags::Nothing ||
                bitsetView.tag == value::TypeTags::valueBlock);

    Date_t endDate;
    TimeUnit unit{TimeUnit::year};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateDiffParameters<true /* isBlockBuiltin */>(
            &endDate, &unit, &timezone, arity == 7 ? &startOfWeek : nullptr)) {
        return makeNothingBlock(valueBlockIn);
    }

    if (unit == TimeUnit::millisecond) {
        auto out = valueBlockIn->map(dateDiffMillisecondOp.bindParams(endDate));
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(out.release())};
    } else {
        auto out = valueBlockIn->map(dateDiffOp.bindParams(endDate, unit, timezone, startOfWeek));
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(out.release())};
    }
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockDateAdd(ArityType arity) {
    tassert(11080028, "Unexpected arity value", arity == 6);

    auto inputView = viewFromStack(1);
    tassert(8649700,
            "Expected input argument to be of valueBlock type",
            inputView.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputView.value);

    auto bitsetView = viewFromStack(0);
    // A bitmap argument set to Nothing is equivalent to a bitmap made of all True values.
    tassert(8649701,
            "Expected bitset argument to be of either Nothing or valueBlock type",
            bitsetView.tag == value::TypeTags::Nothing ||
                bitsetView.tag == value::TypeTags::valueBlock);

    TimeUnit unit{TimeUnit::year};
    int64_t amount;
    TimeZone timezone{};
    if (!validateDateAddParameters<true /* isBlockBuiltin */>(&unit, &amount, &timezone)) {
        return makeNothingBlock(valueBlockIn);
    }

    if (bitsetView.tag == value::TypeTags::valueBlock) {
        // TODO SERVER-86457: refactor this after map() accepts bitmask argument
        auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetView.value);
        auto bitset = bitsetBlock->extract();
        auto bitsetVals = const_cast<value::Value*>(bitset.vals());
        auto bitsetTags = const_cast<value::TypeTags*>(bitset.tags());
        auto valsNum = bitset.count();

        std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
        std::vector<value::Value> valuesOut(valsNum, 0);

        DateAddFunctor dateAddFunc{unit, amount, timezone};
        auto extractedValues = valueBlockIn->extract();

        for (size_t i = 0; i < valsNum; ++i) {
            if (bitsetTags[i] != value::TypeTags::Boolean ||
                !value::bitcastTo<bool>(bitsetVals[i])) {
                continue;
            }

            auto [resTag, resVal] = dateAddFunc(extractedValues[i].tag, extractedValues[i].value);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }

        auto out =
            std::make_unique<value::HeterogeneousBlock>(std::move(tagsOut), std::move(valuesOut));

        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(out.release())};
    } else {
        auto out = valueBlockIn->map(dateAddOp.bindParams(unit, amount, timezone));
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(out.release())};
    }
}

value::TagValueMaybeOwned ByteCode::builtinCurrentDate(ArityType arity) {
    if (MONGO_unlikely(sleepBeforeCurrentDateEvaluationSBE.shouldFail())) {
        sleepBeforeCurrentDateEvaluationSBE.execute(
            [&](const BSONObj& data) { sleepmillis(data["ms"].numberInt()); });
    }

    return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(Date_t::now().asInt64())};
}

}  // namespace mongo::sbe::vm
