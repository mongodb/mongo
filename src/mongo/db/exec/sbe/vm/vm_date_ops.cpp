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
    auto [timezoneDBOwn, timezoneDBTag, timezoneDBValue] = getFromStack(timezoneDBStackPos);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return false;
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBValue);

    size_t stackPosOffset = IsBlockBuiltin ? kStackPosOffsetBlock : 0u;

    auto [endDateOwn, endDateTag, endDateValue] = getFromStack(2 + stackPosOffset);
    if (!coercibleToDate(endDateTag)) {
        return false;
    }
    *endDate = getDate(endDateTag, endDateValue);

    auto [unitOwn, unitTag, unitValue] = getFromStack(3 + stackPosOffset);
    if (!value::isString(unitTag)) {
        return false;
    }
    auto unitString = value::getStringView(unitTag, unitValue);
    if (!isValidTimeUnit(unitString)) {
        return false;
    }
    *unit = parseTimeUnit(unitString);

    // Get timezone.
    auto [timezoneOwned, timezoneTag, timezoneValue] = getFromStack(4 + stackPosOffset);
    if (!isValidTimezone(timezoneTag, timezoneValue, timezoneDB)) {
        return false;
    }
    *timezone = getTimezone(timezoneTag, timezoneValue, timezoneDB);

    // Get startOfWeek, if 'startOfWeek' parameter was requested and time unit is the week.
    if (startOfWeek) {
        auto [startOfWeekOwn, startOfWeekTag, startOfWeekValue] = getFromStack(5 + stackPosOffset);
        if (!value::isString(startOfWeekTag)) {
            return false;
        }
        if (TimeUnit::week == *unit) {
            auto startOfWeekString = value::getStringView(startOfWeekTag, startOfWeekValue);
            if (!isValidDayOfWeek(startOfWeekString)) {
                return false;
            }
            *startOfWeek = parseDayOfWeek(startOfWeekString);
        }
    }
    return true;
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateDiff(ArityType arity) {
    invariant(arity == 5 || arity == 6);  // 6th parameter is 'startOfWeek'.

    Date_t endDate;
    TimeUnit unit{TimeUnit::year};
    TimeZone timezone{};
    DayOfWeek startOfWeek{kStartOfWeekDefault};

    if (!validateDateDiffParameters<>(
            &endDate, &unit, &timezone, arity == 6 ? &startOfWeek : nullptr)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Get startDate.
    auto [startDateOwn, startDateTag, startDateValue] = getFromStack(1);
    if (!coercibleToDate(startDateTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto startDate = getDate(startDateTag, startDateValue);

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
    auto [timezoneDBOwn, timezoneDBTag, timezoneDBVal] = getFromStack(timezoneDBStackPos);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        return false;
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);

    size_t stackPosOffset = IsBlockBuiltin ? kStackPosOffsetBlock : 0u;

    auto [unitOwn, unitTag, unitVal] = getFromStack(2 + stackPosOffset);
    if (!value::isString(unitTag)) {
        return false;
    }
    std::string unitStr{value::getStringView(unitTag, unitVal)};
    if (!isValidTimeUnit(unitStr)) {
        return false;
    }
    *unit = parseTimeUnit(unitStr);

    auto [amountOwn, amountTag, amountVal] = getFromStack(3 + stackPosOffset);
    if (amountTag != value::TypeTags::NumberInt64) {
        return false;
    }
    *amount = value::bitcastTo<int64_t>(amountVal);

    auto [timezoneOwn, timezoneTag, timezoneVal] = getFromStack(4 + stackPosOffset);
    if (!value::isString(timezoneTag) || !isValidTimezone(timezoneTag, timezoneVal, timezoneDB)) {
        return false;
    }
    *timezone = getTimezone(timezoneTag, timezoneVal, timezoneDB);
    return true;
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateAdd(ArityType arity) {
    invariant(arity == 5);

    TimeUnit unit{TimeUnit::year};
    int64_t amount;
    TimeZone timezone{};

    if (!validateDateAddParameters<>(&unit, &amount, &timezone)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [startDateOwn, startDateTag, startDateVal] = getFromStack(1);
    if (!coercibleToDate(startDateTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto startDate = getDate(startDateTag, startDateVal);

    auto resDate = dateAdd(startDate, unit, amount, timezone);
    return {
        false, value::TypeTags::Date, value::bitcastFrom<int64_t>(resDate.toMillisSinceEpoch())};
}

namespace {

/**
 * Used to return a MonoBlock of Nothings. Used when builtinValueBlockDateTrunc receives invalid
 * parameters.
 */
FastTuple<bool, value::TypeTags, value::Value> makeNothingBlock(value::ValueBlock* valueBlockIn) {
    auto out = std::make_unique<value::MonoBlock>(
        valueBlockIn->count(), value::TypeTags::Nothing, value::Value{0u});
    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

struct DateTruncFunctor {
    DateTruncFunctor(TimeUnit unit, int64_t binSize, TimeZone timeZone, DayOfWeek startOfWeek)
        : _unit(unit), _binSize(binSize), _timeZone(timeZone), _startOfWeek(startOfWeek) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (!coercibleToDate(tag)) {
            return std::pair(value::TypeTags::Nothing, value::Value{0u});
        }
        auto date = getDate(tag, val);

        auto truncatedDate = truncateDate(date, _unit, _binSize, _timeZone, _startOfWeek);

        return std::pair(value::TypeTags::Date,
                         value::bitcastFrom<int64_t>(truncatedDate.toMillisSinceEpoch()));
    }

    TimeUnit _unit;
    int64_t _binSize;
    TimeZone _timeZone;
    DayOfWeek _startOfWeek;
};

static const auto dateTruncOp =
    value::makeColumnOpWithParams<ColumnOpType::kMonotonic, DateTruncFunctor>();

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
        auto date = _timeZone.getTimelibTime(getDate(tag, val));

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
        auto date = getDate(tag, val);

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
        auto date = getDate(tag, val);

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

/**
 * Given a ValueBlock and bitset as input, returns a ValueBlock where each date in the input block
 * with corresponding bit set to true have been truncated based on arguments provided. Values that
 * are not coercible to dates are turned into Nothings instead.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockDateTrunc(
    ArityType arity) {
    invariant(arity == 7);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    tassert(8625725,
            "Expected input argument to be of valueBlock type",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    // A bitmap argument set to Nothing is equivalent to a bitmap made of all True values.
    tassert(8625726,
            "Expected bitset argument to be of either Nothing or valueBlock type",
            bitsetTag == value::TypeTags::Nothing || bitsetTag == value::TypeTags::valueBlock);

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

/**
 * Given a ValueBlock and bitset as input, returns a ValueBlock with the difference between each
 * date in the input block with corresponding bit set to true and the argument provided. Values that
 * are not coercible to dates are turned into Nothings instead.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockDateDiff(
    ArityType arity) {
    invariant(arity == 6 || arity == 7);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    tassert(8625727,
            "Expected input argument to be of valueBlock type",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    // A bitmap argument set to Nothing is equivalent to a bitmap made of all True values.
    tassert(8625728,
            "Expected bitset argument to be of either Nothing or valueBlock type",
            bitsetTag == value::TypeTags::Nothing || bitsetTag == value::TypeTags::valueBlock);

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

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockDateAdd(ArityType arity) {
    invariant(arity == 6);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    tassert(8649700,
            "Expected input argument to be of valueBlock type",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    // A bitmap argument set to Nothing is equivalent to a bitmap made of all True values.
    tassert(8649701,
            "Expected bitset argument to be of either Nothing or valueBlock type",
            bitsetTag == value::TypeTags::Nothing || bitsetTag == value::TypeTags::valueBlock);

    TimeUnit unit{TimeUnit::year};
    int64_t amount;
    TimeZone timezone{};
    if (!validateDateAddParameters<true /* isBlockBuiltin */>(&unit, &amount, &timezone)) {
        return makeNothingBlock(valueBlockIn);
    }

    if (bitsetTag == value::TypeTags::valueBlock) {
        // TODO SERVER-86457: refactor this after map() accepts bitmask argument
        auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);
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

            auto [resTag, resVal] =
                dateAddFunc(extractedValues[i].first, extractedValues[i].second);
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
}  // namespace mongo::sbe::vm
