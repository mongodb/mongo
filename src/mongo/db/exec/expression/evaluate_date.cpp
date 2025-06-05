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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(sleepBeforeCurrentDateEvaluation);

namespace {

/**
 * This function checks whether a field is a number.
 *
 * If 'field' is null, the default value is returned trough the 'returnValue' out
 * parameter and the function returns true.
 *
 * If 'field' is not null:
 * - if the value is "nullish", the function returns false.
 * - if the value can not be coerced to an integral value, a UserException is thrown.
 * - otherwise, the coerced integral value is returned through the 'returnValue'
 *   out parameter, and the function returns true.
 */
bool evaluateNumberWithDefault(const Document& root,
                               const Expression* field,
                               StringData fieldName,
                               long long defaultValue,
                               long long* returnValue,
                               Variables* variables) {
    if (!field) {
        *returnValue = defaultValue;
        return true;
    }

    auto fieldValue = field->evaluate(root, variables);

    if (fieldValue.nullish()) {
        return false;
    }

    uassert(40515,
            str::stream() << "'" << fieldName << "' must evaluate to an integer, found "
                          << typeName(fieldValue.getType()) << " with value "
                          << fieldValue.toString(),
            fieldValue.integral64Bit());

    *returnValue = fieldValue.coerceToLong();

    return true;
}

/**
 * This function has the same behavior as evaluteNumberWithDefault(), except that it uasserts if
 * the resulting value is not in the range defined by kMaxValueForDatePart and
 * kMinValueForDatePart.
 */
bool evaluateNumberWithDefaultAndBounds(const Document& root,
                                        const Expression* field,
                                        StringData fieldName,
                                        long long defaultValue,
                                        long long* returnValue,
                                        Variables* variables) {
    // Some date conversions spend a long time iterating through date tables when dealing with large
    // input numbers, so we place a reasonable limit on the magnitude of any argument to
    // $dateFromParts: inputs that fit within a 16-bit int are permitted.
    static constexpr long long kMaxValueForDatePart = std::numeric_limits<int16_t>::max();
    static constexpr long long kMinValueForDatePart = std::numeric_limits<int16_t>::lowest();

    bool result =
        evaluateNumberWithDefault(root, field, fieldName, defaultValue, returnValue, variables);

    uassert(
        31034,
        str::stream() << "'" << fieldName << "'"
                      << " must evaluate to a value in the range [" << kMinValueForDatePart << ", "
                      << kMaxValueForDatePart << "]; value " << *returnValue << " is not in range",
        !result || (*returnValue >= kMinValueForDatePart && *returnValue <= kMaxValueForDatePart));

    return result;
}

boost::optional<int> evaluateIso8601Flag(const Expression* iso8601,
                                         const Document& root,
                                         Variables* variables) {
    if (!iso8601) {
        return false;
    }

    auto iso8601Output = iso8601->evaluate(root, variables);

    if (iso8601Output.nullish()) {
        return boost::none;
    }

    uassert(40521,
            str::stream() << "iso8601 must evaluate to a bool, found "
                          << typeName(iso8601Output.getType()),
            iso8601Output.getType() == BSONType::boolean);

    return iso8601Output.getBool();
}

/**
 * Converts 'value' to Date_t type for $dateDiff expression for parameter 'parameterName'.
 */
Date_t convertDate(const Value& value, StringData expressionName, StringData parameterName) {
    uassert(5166307,
            str::stream() << expressionName << " requires '" << parameterName
                          << "' to be a date, but got " << typeName(value.getType()),
            value.coercibleToDate());
    return value.coerceToDate();
}

}  // namespace

namespace exec::expression {

TimeUnit parseTimeUnit(const Value& value, StringData expressionName) {
    uassert(5439013,
            str::stream() << expressionName << " requires 'unit' to be a string, but got "
                          << typeName(value.getType()),
            BSONType::string == value.getType());
    return addContextToAssertionException([&]() { return parseTimeUnit(value.getStringData()); },
                                          expressionName,
                                          " parameter 'unit' value parsing failed"_sd);
}

DayOfWeek parseDayOfWeek(const Value& value, StringData expressionName, StringData parameterName) {
    uassert(5439015,
            str::stream() << expressionName << " requires '" << parameterName
                          << "' to be a string, but got " << typeName(value.getType()),
            BSONType::string == value.getType());
    uassert(5439016,
            str::stream() << expressionName << " parameter '" << parameterName
                          << "' value cannot be recognized as a day of a week: "
                          << value.getStringData(),
            isValidDayOfWeek(value.getStringData()));
    return parseDayOfWeek(value.getStringData());
}

boost::optional<TimeZone> makeTimeZone(const TimeZoneDatabase* tzdb,
                                       const Document& root,
                                       const Expression* timeZone,
                                       Variables* variables) {
    invariant(tzdb);

    if (!timeZone) {
        return mongo::TimeZoneDatabase::utcZone();
    }

    auto timeZoneId = timeZone->evaluate(root, variables);

    if (timeZoneId.nullish()) {
        return boost::none;
    }

    uassert(40517,
            str::stream() << "timezone must evaluate to a string, found "
                          << typeName(timeZoneId.getType()),
            timeZoneId.getType() == BSONType::string);

    return tzdb->getTimeZone(timeZoneId.getStringData());
}

unsigned long long convertDateTruncBinSizeValue(const Value& value) {
    uassert(5439017,
            str::stream() << "$dateTrunc requires 'binSize' to be a 64-bit integer, but got value '"
                          << value.toString() << "' of type " << typeName(value.getType()),
            value.integral64Bit());
    const long long binSize = value.coerceToLong();
    uassert(5439018,
            str::stream() << "$dateTrunc requires 'binSize' to be greater than 0, but got value "
                          << binSize,
            binSize > 0);
    return static_cast<unsigned long long>(binSize);
}

Value evaluate(const ExpressionDateFromParts& expr, const Document& root, Variables* variables) {
    long long hour, minute, second, millisecond;

    if (!evaluateNumberWithDefaultAndBounds(root, expr.getHour(), "hour"_sd, 0, &hour, variables) ||
        !evaluateNumberWithDefaultAndBounds(
            root, expr.getMinute(), "minute"_sd, 0, &minute, variables) ||
        !evaluateNumberWithDefault(root, expr.getSecond(), "second"_sd, 0, &second, variables) ||
        !evaluateNumberWithDefault(
            root, expr.getMillisecond(), "millisecond"_sd, 0, &millisecond, variables)) {
        // One of the evaluated inputs in nullish.
        return Value(BSONNULL);
    }

    boost::optional<TimeZone> timeZone = expr.getParsedTimeZone();
    if (!timeZone) {
        timeZone = makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                root,
                                expr.getTimeZone(),
                                variables);
        if (!timeZone) {
            return Value(BSONNULL);
        }
    }

    if (expr.getYear()) {
        long long year, month, day;

        if (!evaluateNumberWithDefault(root, expr.getYear(), "year"_sd, 1970, &year, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, expr.getMonth(), "month"_sd, 1, &month, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, expr.getDay(), "day"_sd, 1, &day, variables)) {
            // One of the evaluated inputs in nullish.
            return Value(BSONNULL);
        }

        uassert(40523,
                str::stream() << "'year' must evaluate to an integer in the range " << 1 << " to "
                              << 9999 << ", found " << year,
                year >= 1 && year <= 9999);

        return Value(
            timeZone->createFromDateParts(year, month, day, hour, minute, second, millisecond));
    }

    if (expr.getIsoWeekYear()) {
        long long isoWeekYear, isoWeek, isoDayOfWeek;

        if (!evaluateNumberWithDefault(
                root, expr.getIsoWeekYear(), "isoWeekYear"_sd, 1970, &isoWeekYear, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, expr.getIsoWeek(), "isoWeek"_sd, 1, &isoWeek, variables) ||
            !evaluateNumberWithDefaultAndBounds(
                root, expr.getIsoDayOfWeek(), "isoDayOfWeek"_sd, 1, &isoDayOfWeek, variables)) {
            // One of the evaluated inputs in nullish.
            return Value(BSONNULL);
        }

        uassert(31095,
                str::stream() << "'isoWeekYear' must evaluate to an integer in the range " << 1
                              << " to " << 9999 << ", found " << isoWeekYear,
                isoWeekYear >= 1 && isoWeekYear <= 9999);

        return Value(timeZone->createFromIso8601DateParts(
            isoWeekYear, isoWeek, isoDayOfWeek, hour, minute, second, millisecond));
    }

    MONGO_UNREACHABLE;
}

Value evaluate(const ExpressionDateFromString& expr, const Document& root, Variables* variables) {
    const Value dateString = expr.getDateString()->evaluate(root, variables);
    Value formatValue;

    // Eagerly validate the format parameter, ignoring if nullish since the input string nullish
    // behavior takes precedence.
    if (expr.getFormat()) {
        formatValue = expr.getFormat()->evaluate(root, variables);
        if (!formatValue.nullish()) {
            uassert(40684,
                    str::stream() << "$dateFromString requires that 'format' be a string, found: "
                                  << typeName(formatValue.getType()) << " with value "
                                  << formatValue.toString(),
                    formatValue.getType() == BSONType::string);

            TimeZone::validateFromStringFormat(formatValue.getStringData());
        }
    }

    // Evaluate the timezone parameter before checking for nullish input, as this will throw an
    // exception for an invalid timezone string.
    boost::optional<TimeZone> timeZone = expr.getParsedTimeZone();
    if (!timeZone) {
        timeZone = makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                root,
                                expr.getTimeZone(),
                                variables);
    }

    // Behavior for nullish input takes precedence over other nullish elements.
    if (dateString.nullish()) {
        return expr.getOnNull() ? expr.getOnNull()->evaluate(root, variables) : Value(BSONNULL);
    }

    try {
        uassert(ErrorCodes::ConversionFailure,
                str::stream() << "$dateFromString requires that 'dateString' be a string, found: "
                              << typeName(dateString.getType()) << " with value "
                              << dateString.toString(),
                dateString.getType() == BSONType::string);

        const auto dateTimeString = dateString.getStringData();

        if (!timeZone) {
            return Value(BSONNULL);
        }

        if (expr.getFormat()) {
            if (formatValue.nullish()) {
                return Value(BSONNULL);
            }

            return Value(expr.getExpressionContext()->getTimeZoneDatabase()->fromString(
                dateTimeString, timeZone.value(), formatValue.getStringData()));
        }

        return Value(expr.getExpressionContext()->getTimeZoneDatabase()->fromString(
            dateTimeString, timeZone.value()));
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (expr.getOnError()) {
            return expr.getOnError()->evaluate(root, variables);
        }
        throw;
    }
}

Value evaluate(const ExpressionDateToParts& expr, const Document& root, Variables* variables) {
    tassert(9711500, "Missing date argument", expr.getDate() != nullptr);
    const Value date = expr.getDate()->evaluate(root, variables);

    boost::optional<TimeZone> timeZone = expr.getParsedTimeZone();
    if (!timeZone) {
        timeZone = makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                root,
                                expr.getTimeZone(),
                                variables);
        if (!timeZone) {
            return Value(BSONNULL);
        }
    }

    auto iso8601 = evaluateIso8601Flag(expr.getIso8601(), root, variables);
    if (!iso8601) {
        return Value(BSONNULL);
    }

    if (date.nullish()) {
        return Value(BSONNULL);
    }

    auto dateValue = date.coerceToDate();

    if (*iso8601) {
        auto parts = timeZone->dateIso8601Parts(dateValue);
        return Value(Document{{"isoWeekYear", parts.year},
                              {"isoWeek", parts.weekOfYear},
                              {"isoDayOfWeek", parts.dayOfWeek},
                              {"hour", parts.hour},
                              {"minute", parts.minute},
                              {"second", parts.second},
                              {"millisecond", parts.millisecond}});
    } else {
        auto parts = timeZone->dateParts(dateValue);
        return Value(Document{{"year", parts.year},
                              {"month", parts.month},
                              {"day", parts.dayOfMonth},
                              {"hour", parts.hour},
                              {"minute", parts.minute},
                              {"second", parts.second},
                              {"millisecond", parts.millisecond}});
    }
}

Value evaluate(const ExpressionDateToString& expr, const Document& root, Variables* variables) {
    tassert(9711501, "Missing date argument", expr.getDate() != nullptr);
    const Value date = expr.getDate()->evaluate(root, variables);
    Value formatValue;

    // Eagerly validate the format parameter, ignoring if nullish since the input date nullish
    // behavior takes precedence.
    if (expr.getFormat()) {
        formatValue = expr.getFormat()->evaluate(root, variables);
        if (!formatValue.nullish()) {
            uassert(18533,
                    str::stream() << "$dateToString requires that 'format' be a string, found: "
                                  << typeName(formatValue.getType()) << " with value "
                                  << formatValue.toString(),
                    formatValue.getType() == BSONType::string);

            TimeZone::validateToStringFormat(formatValue.getStringData());
        }
    }

    // Evaluate the timezone parameter before checking for nullish input, as this will throw an
    // exception for an invalid timezone string.
    boost::optional<TimeZone> timeZone = expr.getParsedTimeZone();
    if (!timeZone) {
        timeZone = makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                root,
                                expr.getTimeZone(),
                                variables);
    }

    if (date.nullish()) {
        return expr.getOnNull() ? expr.getOnNull()->evaluate(root, variables) : Value(BSONNULL);
    }

    if (!timeZone) {
        return Value(BSONNULL);
    }

    if (expr.getFormat()) {
        if (formatValue.nullish()) {
            return Value(BSONNULL);
        }

        return Value(uassertStatusOK(
            timeZone->formatDate(formatValue.getStringData(), date.coerceToDate())));
    }

    return Value(uassertStatusOK(timeZone->formatDate(
        timeZone->isUtcZone() ? kIsoFormatStringZ : kIsoFormatStringNonZ, date.coerceToDate())));
}

Value evaluate(const ExpressionDateDiff& expr, const Document& root, Variables* variables) {
    const Value startDateValue = expr.getStartDate()->evaluate(root, variables);
    if (startDateValue.nullish()) {
        return Value(BSONNULL);
    }
    const Value endDateValue = expr.getEndDate()->evaluate(root, variables);
    if (endDateValue.nullish()) {
        return Value(BSONNULL);
    }

    TimeUnit unit;
    if (expr.getParsedUnit()) {
        unit = *expr.getParsedUnit();
    } else {
        const Value unitValue = expr.getUnit()->evaluate(root, variables);
        if (unitValue.nullish()) {
            return Value(BSONNULL);
        }
        unit = parseTimeUnit(unitValue, "$dateDiff"_sd);
    }

    DayOfWeek startOfWeek = kStartOfWeekDefault;
    if (unit == TimeUnit::week) {
        if (expr.getParsedStartOfWeek()) {
            startOfWeek = *expr.getParsedStartOfWeek();
        } else if (expr.getStartOfWeek()) {
            const Value startOfWeekValue = expr.getStartOfWeek()->evaluate(root, variables);
            if (startOfWeekValue.nullish()) {
                return Value(BSONNULL);
            }
            startOfWeek = parseDayOfWeek(startOfWeekValue, "$dateDiff"_sd, "startOfWeek"_sd);
        }
    }

    boost::optional<TimeZone> timezone = expr.getParsedTimeZone();
    if (!timezone) {
        timezone = addContextToAssertionException(
            [&]() {
                return makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                    root,
                                    expr.getTimeZone(),
                                    variables);
            },
            "$dateDiff parameter 'timezone' value parsing failed"_sd);
        if (!timezone) {
            return Value(BSONNULL);
        }
    }

    const Date_t startDate = convertDate(startDateValue, "$dateDiff"_sd, "startDate"_sd);
    const Date_t endDate = convertDate(endDateValue, "$dateDiff"_sd, "endDate"_sd);
    return Value{dateDiff(startDate, endDate, unit, *timezone, startOfWeek)};
}

namespace {

// Common code shared between DateAdd and DateSubtract implementations. It converts the parameters
// of the function into their native C++ types. It returns false if at least one of the parameters
// is Null, and throws an AssertionException if any of them is an invalid value.
bool extractDateArithmetics(const ExpressionDateArithmetics& expr,
                            const Document& root,
                            Variables* variables,
                            Date_t& outDate,
                            TimeUnit& unit,
                            long long& outAmount,
                            boost::optional<TimeZone>& timezone) {
    const Value startDate = expr.getStartDate()->evaluate(root, variables);
    if (startDate.nullish()) {
        return false;
    }

    if (expr.getParsedUnit()) {
        unit = *expr.getParsedUnit();
    } else {
        const Value unitVal = expr.getUnit()->evaluate(root, variables);
        if (unitVal.nullish()) {
            return false;
        }
        unit = parseTimeUnit(unitVal, expr.getOpName());
    }

    auto amount = expr.getAmount()->evaluate(root, variables);
    if (amount.nullish()) {
        return false;
    }

    // Get the TimeZone object for the timezone parameter, if it is specified, or UTC otherwise.
    timezone = expr.getParsedTimeZone();
    if (!timezone) {
        timezone = makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                root,
                                expr.getTimeZone(),
                                variables);
        if (!timezone) {
            return false;
        }
    }

    uassert(5166403,
            str::stream() << expr.getOpName() << " requires startDate to be convertible to a date",
            startDate.coercibleToDate());
    uassert(5166405,
            str::stream() << expr.getOpName() << " expects integer amount of time units",
            amount.integral64Bit());

    outDate = startDate.coerceToDate();
    outAmount = amount.coerceToLong();
    return true;
}

}  // namespace

Value evaluate(const ExpressionDateAdd& expr, const Document& root, Variables* variables) {
    Date_t date;
    TimeUnit unit;
    long long amount;
    boost::optional<TimeZone> timezone;
    if (!extractDateArithmetics(expr, root, variables, date, unit, amount, timezone)) {
        return Value(BSONNULL);
    }
    return Value(dateAdd(date, unit, amount, *timezone));
}

Value evaluate(const ExpressionDateSubtract& expr, const Document& root, Variables* variables) {
    Date_t date;
    TimeUnit unit;
    long long amount;
    boost::optional<TimeZone> timezone;
    if (!extractDateArithmetics(expr, root, variables, date, unit, amount, timezone)) {
        return Value(BSONNULL);
    }
    // Long long min value cannot be negated.
    uassert(6045000,
            str::stream() << "invalid $dateSubtract 'amount' parameter value: " << amount,
            amount != std::numeric_limits<long long>::min());
    return Value(dateAdd(date, unit, -amount, *timezone));
}

Value evaluate(const ExpressionDateTrunc& expr, const Document& root, Variables* variables) {
    tassert(9711502, "Missing date argument", expr.getDate() != nullptr);
    const Value dateValue = expr.getDate()->evaluate(root, variables);
    if (dateValue.nullish()) {
        return Value(BSONNULL);
    }

    unsigned long long binSize = 1;
    if (expr.getParsedBinSize()) {
        binSize = *expr.getParsedBinSize();
    } else if (expr.getBinSize()) {
        const Value binSizeValue = expr.getBinSize()->evaluate(root, variables);
        if (binSizeValue.nullish()) {
            return Value(BSONNULL);
        }
        binSize = convertDateTruncBinSizeValue(binSizeValue);
    }

    TimeUnit unit;
    if (expr.getParsedUnit()) {
        unit = *expr.getParsedUnit();
    } else {
        const Value unitValue = expr.getUnit()->evaluate(root, variables);
        if (unitValue.nullish()) {
            return Value(BSONNULL);
        }
        unit = parseTimeUnit(unitValue, "$dateTrunc"_sd);
    }

    DayOfWeek startOfWeek = kStartOfWeekDefault;
    if (unit == TimeUnit::week) {
        if (expr.getParsedStartOfWeek()) {
            startOfWeek = *expr.getParsedStartOfWeek();
        } else if (expr.getStartOfWeek()) {
            const Value startOfWeekValue = expr.getStartOfWeek()->evaluate(root, variables);
            if (startOfWeekValue.nullish()) {
                return Value(BSONNULL);
            }
            startOfWeek = parseDayOfWeek(startOfWeekValue, "$dateTrunc"_sd, "startOfWeek"_sd);
        }
    }

    boost::optional<TimeZone> timezone = expr.getParsedTimeZone();
    if (!timezone) {
        timezone = addContextToAssertionException(
            [&]() {
                return makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                                    root,
                                    expr.getTimeZone(),
                                    variables);
            },
            "$dateTrunc parameter 'timezone' value parsing failed"_sd);
        if (!timezone) {
            return Value(BSONNULL);
        }
    }

    // Convert parameter values.
    const Date_t date = convertDate(dateValue, "$dateTrunc"_sd, "date"_sd);
    return Value{truncateDate(date, unit, binSize, *timezone, startOfWeek)};
}

Value evaluate(const ExpressionTsSecond& expr, const Document& root, Variables* variables) {
    const Value operand = expr.getOperandList()[0]->evaluate(root, variables);

    if (operand.nullish()) {
        return Value(BSONNULL);
    }

    uassert(5687301,
            str::stream() << " Argument to " << expr.getOpName() << " must be a timestamp, but is "
                          << typeName(operand.getType()),
            operand.getType() == BSONType::timestamp);

    return Value(static_cast<long long>(operand.getTimestamp().getSecs()));
}

Value evaluate(const ExpressionTsIncrement& expr, const Document& root, Variables* variables) {
    const Value operand = expr.getOperandList()[0]->evaluate(root, variables);

    if (operand.nullish()) {
        return Value(BSONNULL);
    }

    uassert(5687302,
            str::stream() << " Argument to " << expr.getOpName() << " must be a timestamp, but is "
                          << typeName(operand.getType()),
            operand.getType() == BSONType::timestamp);

    return Value(static_cast<long long>(operand.getTimestamp().getInc()));
}

namespace {

// Common code shared between all the single-argument date extraction functions. It converts the
// parameters of the function into their native C++ types, throwing an AssertionException if any of
// them is an invalid value. It then invokes the provided lambda function on the computed date and
// timezone.
template <typename dateFuncFn>
Value evaluateDateTimezoneArg(const DateExpressionAcceptingTimeZone& expr,
                              const Document& root,
                              Variables* variables,
                              dateFuncFn dateFn) {
    tassert(9711503, "Missing date argument", expr.getDate() != nullptr);
    auto dateVal = expr.getDate()->evaluate(root, variables);
    if (dateVal.nullish()) {
        return Value(BSONNULL);
    }
    auto date = dateVal.coerceToDate();

    if (expr.getParsedTimeZone()) {
        return dateFn(date, *expr.getParsedTimeZone());
    } else {
        boost::optional<TimeZone> timeZone =
            makeTimeZone(expr.getExpressionContext()->getTimeZoneDatabase(),
                         root,
                         expr.getTimeZone(),
                         variables);
        if (!timeZone) {
            return Value(BSONNULL);
        } else {
            return dateFn(date, *timeZone);
        }
    }
}
}  // namespace

Value evaluate(const ExpressionDayOfMonth& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).dayOfMonth);
        });
}

Value evaluate(const ExpressionDayOfWeek& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dayOfWeek(date));
        });
}

Value evaluate(const ExpressionDayOfYear& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dayOfYear(date));
        });
}

Value evaluate(const ExpressionHour& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).hour);
        });
}

Value evaluate(const ExpressionMillisecond& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).millisecond);
        });
}

Value evaluate(const ExpressionMinute& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).minute);
        });
}

Value evaluate(const ExpressionMonth& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).month);
        });
}

Value evaluate(const ExpressionSecond& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).second);
        });
}

Value evaluate(const ExpressionWeek& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.week(date));
        });
}

Value evaluate(const ExpressionIsoWeekYear& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.isoYear(date));
        });
}

Value evaluate(const ExpressionIsoDayOfWeek& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.isoDayOfWeek(date));
        });
}

Value evaluate(const ExpressionIsoWeek& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.isoWeek(date));
        });
}

Value evaluate(const ExpressionYear& expr, const Document& root, Variables* variables) {
    return evaluateDateTimezoneArg(
        expr, root, variables, [](Date_t date, const TimeZone& timeZone) {
            return Value(timeZone.dateParts(date).year);
        });
}

Value evaluate(const ExpressionCurrentDate&, const Document&, Variables*) {
    if (MONGO_unlikely(sleepBeforeCurrentDateEvaluation.shouldFail())) {
        sleepBeforeCurrentDateEvaluation.execute(
            [&](const BSONObj& data) { sleepmillis(data["ms"].numberInt()); });
    }

    return Value(Date_t::now());
}

}  // namespace exec::expression
}  // namespace mongo
