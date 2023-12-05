/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"

#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe::vm {
using ColumnOpType = value::ColumnOpType;

namespace {
const size_t kTimezoneDBStackPosDefault = 0u;
const size_t kTimezoneDBStackPosBlock = 2u;
const size_t kStackPosOffsetBlock = 1u;
}  // namespace

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
    auto [timezoneDBOwn, timezoneDBTag, timezoneDBValue] = getFromStack(timezoneDBStackPos);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return false;
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBValue);

    size_t stackPosOffset = IsBlockBuiltin ? kStackPosOffsetBlock : 0u;

    auto [unitOwn, unitTag, unitValue] = getFromStack(2 + stackPosOffset);
    if (!value::isString(unitTag)) {
        return false;
    }
    auto unitString = value::getStringView(unitTag, unitValue);
    if (!isValidTimeUnit(unitString)) {
        return false;
    }
    *unit = parseTimeUnit(unitString);

    // Get binSize.
    auto [binSizeOwned, binSizeTag, binSizeValue] = getFromStack(3 + stackPosOffset);
    if (!value::isNumber(binSizeTag)) {
        return false;
    }
    auto [binSizeLongOwned, binSizeLongTag, binSizeLongValue] =
        value::genericNumConvert(binSizeTag, binSizeValue, value::TypeTags::NumberInt64);
    if (binSizeLongTag == value::TypeTags::Nothing) {
        return false;
    }
    *binSize = value::bitcastTo<int64_t>(binSizeLongValue);
    if (*binSize <= 0) {
        return false;
    }

    // Get timezone.
    auto [timezoneOwned, timezoneTag, timezoneValue] = getFromStack(4 + stackPosOffset);
    if (!isValidTimezone(timezoneTag, timezoneValue, timezoneDB)) {
        return false;
    }
    *timezone = getTimezone(timezoneTag, timezoneValue, timezoneDB);

    // Get startOfWeek, if 'startOfWeek' parameter was passed and time unit is the week.
    if (*unit == TimeUnit::week) {
        auto [startOfWeekOwned, startOfWeekTag, startOfWeekValue] =
            getFromStack(5 + stackPosOffset);
        if (!value::isString(startOfWeekTag)) {
            return false;
        }
        auto startOfWeekString = value::getStringView(startOfWeekTag, startOfWeekValue);
        if (!isValidDayOfWeek(startOfWeekString)) {
            return false;
        }
        *startOfWeek = parseDayOfWeek(startOfWeekString);
    }

    return true;
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateTrunc(ArityType arity) {
    invariant(arity == 6);

    TimeUnit unit{TimeUnit::year};
    int64_t binSize{0u};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateTruncParameters<>(&unit, &binSize, &timezone, &startOfWeek)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Get date.
    auto [dateOwn, dateTag, dateValue] = getFromStack(1);

    return dateTrunc(dateTag, dateValue, unit, binSize, timezone, startOfWeek);
}

namespace {

/**
 * Used to return a MonoBlock of Nothings. Used when builtinValueBlockDateTrunc receives invalid
 * parameters.
 */
FastTuple<bool, value::TypeTags, value::Value> makeNothingBlock(value::ValueBlock* valueBlockIn) {
    auto count = valueBlockIn->tryCount();
    if (!count) {
        count = valueBlockIn->extract().count;
    }
    auto out =
        std::make_unique<value::MonoBlock>(*count, value::TypeTags::Nothing, value::Value{0u});
    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

struct DateTruncFunctor {
    DateTruncFunctor(TimeUnit unit, int64_t binSize, TimeZone timezone, DayOfWeek startOfWeek)
        : _unit(unit), _binSize(binSize), _timezone(timezone), _startOfWeek(startOfWeek) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = getDate(tag, val);

        auto truncatedDate = truncateDate(date, _unit, _binSize, _timezone, _startOfWeek);

        return std::pair(value::TypeTags::Date,
                         value::bitcastFrom<int64_t>(truncatedDate.toMillisSinceEpoch()));
    }

    TimeUnit _unit;
    int64_t _binSize;
    TimeZone _timezone;
    DayOfWeek _startOfWeek;
};

static constexpr auto dateTruncOpType =
    ColumnOpType{ColumnOpType::kMonotonic | ColumnOpType::kOutputNonNothingOnExpectedInput,
                 value::TypeTags::Date,
                 value::TypeTags::Nothing,
                 ColumnOpType::ReturnNothingOnMissing{}};

static const auto dateTruncOp = value::makeColumnOpWithParams<dateTruncOpType, DateTruncFunctor>();
}  // namespace

/**
 * Given a ValueBlock and bitset as input, returns a ValueBlock where each date in the input block
 * with corresponding bit set to true have been truncated based on arguments provided. Values that
 * are not coercible to dates are turned into Nothings instead.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockDateTrunc(
    ArityType arity) {
    invariant(arity == 7);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    invariant(bitsetTag == value::TypeTags::valueBlock);

    TimeUnit unit{TimeUnit::year};
    int64_t binSize{0u};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateTruncParameters<true /* isBlockBuiltin */>(
            &unit, &binSize, &timezone, &startOfWeek)) {
        return makeNothingBlock(valueBlockIn);
    }

    auto out = valueBlockIn->map(dateTruncOp.bindParams(unit, binSize, timezone, startOfWeek));

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

}  // namespace mongo::sbe::vm
