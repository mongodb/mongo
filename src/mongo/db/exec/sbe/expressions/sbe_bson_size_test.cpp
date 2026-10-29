// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sbe {
using SBEBsonSizeTest = EExpressionTestFixture;

TEST_F(SBEBsonSizeTest, ComputesSizeForBsonDocument) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>(EFn::kBsonSize, sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    BSONObjBuilder objBuilder;
    objBuilder.append("name", "Test string element");
    objBuilder.append("age", 32);
    objBuilder.append("citizen", true);
    auto bsonObj = objBuilder.done();

    slotAccessor.reset(value::TypeTags::bsonObject,
                       value::bitcastFrom<const char*>(bsonObj.objdata()));
    value::TagValueOwned size =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQUALS(value::TypeTags::NumberInt32, size.tag());
    ASSERT_EQUALS(value::bitcastTo<int32_t>(size.value()), bsonObj.objsize());
}

TEST_F(SBEBsonSizeTest, ComputesSizeForSbeObject) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>(EFn::kBsonSize, sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    auto [tagArg1, valArg1] = value::makeNewString("Test string element");
    value::TagValueOwned obj = value::TagValueOwned::fromRaw(value::makeNewObject());
    auto objView = value::getObjectView(obj.value());
    objView->push_back_raw("name", tagArg1, valArg1);
    objView->push_back_raw("age", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(32));
    objView->push_back_raw("citizen", value::TypeTags::Boolean, value::bitcastFrom<bool>(true));

    slotAccessor.reset(value::TypeTags::Object, obj.value());
    value::TagValueOwned size =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQUALS(value::TypeTags::NumberInt32, size.tag());
    ASSERT_EQUALS(value::bitcastTo<int32_t>(size.value()), 54);
}

TEST_F(SBEBsonSizeTest, ComputesSizeForLargeSbeObject) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>(EFn::kBsonSize, sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    // Four 9MiB strings make an SBE object that serializes to more than 16MiB of BSON.
    constexpr size_t longStringLength = 9 * 1024 * 1024;
    static_assert(4 * longStringLength > BSONObjMaxUserSize);

    auto [tagStr1, valStr1] = value::makeNewString(std::string(longStringLength, 'A'));
    auto [tagStr2, valStr2] = value::makeNewString(std::string(longStringLength, 'B'));
    auto [tagStr3, valStr3] = value::makeNewString(std::string(longStringLength, 'C'));
    auto [tagStr4, valStr4] = value::makeNewString(std::string(longStringLength, 'D'));
    value::TagValueOwned obj = value::TagValueOwned::fromRaw(value::makeNewObject());
    auto objView = value::getObjectView(obj.value());
    objView->push_back_raw("a", tagStr1, valStr1);
    objView->push_back_raw("b", tagStr2, valStr2);
    objView->push_back_raw("c", tagStr3, valStr3);
    objView->push_back_raw("d", tagStr4, valStr4);

    slotAccessor.reset(value::TypeTags::Object, obj.value());
    value::TagValueOwned size =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    // Must not throw BSONObjectTooLarge for objects larger than 16MiB.
    ASSERT_EQUALS(value::TypeTags::NumberInt32, size.tag());
    ASSERT_GT(value::bitcastTo<int32_t>(size.value()), static_cast<int32_t>(BSONObjMaxUserSize));
}

TEST_F(SBEBsonSizeTest, ReturnsNothingForNonObject) {
    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto bsonSizeExpr =
        sbe::makeE<sbe::EFunction>(EFn::kBsonSize, sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*bsonSizeExpr);

    value::TagValueOwned str =
        value::TagValueOwned::fromRaw(value::makeNewString("Test string element"));

    value::TagValueOwned arr = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arrView = value::getArrayView(arr.value());
    arrView->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(32));
    auto [tagStrCopy, valStrCopy] = value::copyValue(str.tag(), str.value());
    arrView->push_back_raw(tagStrCopy, valStrCopy);

    std::vector<std::pair<value::TypeTags, value::Value>> testData = {
        std::make_pair(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(12789)),
        std::make_pair(str.tag(), str.value()),
        std::make_pair(arr.tag(), arr.value())};

    for (size_t i = 0; i < testData.size(); i++) {
        slotAccessor.reset(testData[i].first, testData[i].second);
        value::TagValueOwned size =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
        ASSERT_EQUALS(value::TypeTags::Nothing, size.tag());
    }
}

}  // namespace mongo::sbe
