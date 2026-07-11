// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_view.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <tuple>

namespace mongo::sbe {

class SBEBuiltinDateAddTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, int64_t expectedDate) {
        value::TagValueOwned dateResult =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT_EQUALS(dateResult.tag(), sbe::value::TypeTags::Date);
        ASSERT_EQ(value::bitcastTo<int64_t>(dateResult.value()), expectedDate);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned nothingResult =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(nothingResult.tag(), sbe::value::TypeTags::Nothing);
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

    auto dateAddExpr = sbe::makeE<sbe::EFunction>(EFn::kDateAdd,
                                                  sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                              makeE<EVariable>(startDateSlot),
                                                              makeE<EVariable>(unitSlot),
                                                              makeE<EVariable>(amountSlot),
                                                              makeE<EVariable>(timezoneSlot)));
    auto compiledExpr = compileExpression(*dateAddExpr);

    int64_t startInstant = 1435006000;
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(value::TagValueView{
        value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get())});
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

    auto dateAddExpr = sbe::makeE<sbe::EFunction>(EFn::kDateAdd,
                                                  sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                              makeE<EVariable>(startDateSlot),
                                                              makeE<EVariable>(unitSlot),
                                                              makeE<EVariable>(amountSlot),
                                                              makeE<EVariable>(timezoneSlot)));
    auto compiledExpr = compileExpression(*dateAddExpr);

    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(value::TagValueView{
        value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get())});

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
