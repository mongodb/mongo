/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/query/datetime/date_time_support.h"

namespace mongo::sbe {

class SBEBuiltinDateAddTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, int64_t expectedDate) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(tag, sbe::value::TypeTags::Date);
        ASSERT_EQ(value::bitcastTo<int64_t>(val), expectedDate);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(tag, sbe::value::TypeTags::Nothing);
    }
};

TEST_F(SBEBuiltinDateAddTest, ComputesDateAdd) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor startDateAccessor;
    auto startDateSlot = bindAccessor(&startDateAccessor);
    value::OwnedValueAccessor unitAccessor;
    auto unitSlot = bindAccessor(&unitAccessor);
    value::OwnedValueAccessor amountAccessor;
    auto amountSlot = bindAccessor(&amountAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dateAddExpr = sbe::makeE<sbe::EFunction>("dateAdd",
                                                  sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                              makeE<EVariable>(startDateSlot),
                                                              makeE<EVariable>(unitSlot),
                                                              makeE<EVariable>(amountSlot),
                                                              makeE<EVariable>(timezoneSlot)));
    auto compiledExpr = compileExpression(*dateAddExpr);

    int64_t startInstant = 1435006000;
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    startDateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(startInstant));
    auto [unitTag, unitVal] = value::makeNewString("minute");
    unitAccessor.reset(unitTag, unitVal);
    amountAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10));
    auto [tzTag, tzVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(tzTag, tzVal);
    runAndAssertExpression(compiledExpr.get(), startInstant + 10 * 60 * 1000L);

    std::tie(unitTag, unitVal) = value::makeNewString("hour");
    unitAccessor.reset(unitTag, unitVal);
    runAndAssertExpression(compiledExpr.get(), startInstant + 10 * 60 * 60 * 1000L);

    // startDate of type Timestamp.
    const Timestamp ts{Seconds(2000), 0};
    startDateAccessor.reset(value::TypeTags::Timestamp, value::bitcastFrom<int64_t>(ts.asULL()));
    std::tie(unitTag, unitVal) = value::makeNewString("second");
    unitAccessor.reset(unitTag, unitVal);
    runAndAssertExpression(compiledExpr.get(), (2000 + 10) * 1000L);

    // startDate of type ObjectId.
    const OID oid = OID::gen();
    auto [oidTag, oidVal] = value::makeNewObjectId();
    oid.view().readInto(value::getObjectIdView(oidVal));
    startDateAccessor.reset(oidTag, oidVal);
    amountAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0));
    runAndAssertExpression(compiledExpr.get(), oid.asDateT().toMillisSinceEpoch());
}

TEST_F(SBEBuiltinDateAddTest, ReturnsNothingDateAdd) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor startDateAccessor;
    auto startDateSlot = bindAccessor(&startDateAccessor);
    value::OwnedValueAccessor unitAccessor;
    auto unitSlot = bindAccessor(&unitAccessor);
    value::OwnedValueAccessor amountAccessor;
    auto amountSlot = bindAccessor(&amountAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dateAddExpr = sbe::makeE<sbe::EFunction>("dateAdd",
                                                  sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                              makeE<EVariable>(startDateSlot),
                                                              makeE<EVariable>(unitSlot),
                                                              makeE<EVariable>(amountSlot),
                                                              makeE<EVariable>(timezoneSlot)));
    auto compiledExpr = compileExpression(*dateAddExpr);

    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));

    // Invalid startDate.
    auto [invalidDateTag, invalidDateVal] = value::makeNewString("my birthday");
    startDateAccessor.reset(invalidDateTag, invalidDateVal);
    auto [unitTag, unitVal] = value::makeNewString("minute");
    unitAccessor.reset(unitTag, unitVal);
    amountAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10));
    auto [tzTag, tzVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(tzTag, tzVal);
    runAndAssertNothing(compiledExpr.get());

    startDateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(1435006000));
    // Invalid unit.
    unitAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(123));
    runAndAssertNothing(compiledExpr.get());
    std::tie(unitTag, unitVal) = value::makeNewString("workday");
    unitAccessor.reset(unitTag, unitVal);
    runAndAssertNothing(compiledExpr.get());

    std::tie(unitTag, unitVal) = value::makeNewString("day");
    unitAccessor.reset(unitTag, unitVal);
    // Invalid amount.
    amountAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.05));
    runAndAssertNothing(compiledExpr.get());

    amountAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10));
    // Invalid timezone.
    std::tie(tzTag, tzVal) = value::makeNewString("Undefined");
    timezoneAccessor.reset(tzTag, tzVal);
    runAndAssertNothing(compiledExpr.get());
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(180));
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
