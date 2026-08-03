// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace mongo::sbe {
class SBENewObjTest : public EExpressionTestFixture {
protected:
    /**
     * Runs 'compiledExpr' and asserts it produced an object. Returns a non-owning view of it; the
     * returned pointer stays valid for the lifetime of 'guard'.
     */
    value::Object* runAndGetObject(const vm::CodeFragment* compiledExpr, value::ValueGuard& guard) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        guard = value::ValueGuard{tag, val};

        ASSERT_EQUALS(value::TypeTags::Object, tag);
        return value::getObjectView(val);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(value::TypeTags::Nothing, tag);
    }
};

TEST_F(SBENewObjTest, BuildsEmptyObjectFromNoArguments) {
    auto expr = sbe::makeE<sbe::EFunction>(EFn::kNewObj, sbe::makeEs());
    auto compiledExpr = compileExpression(*expr);

    value::ValueGuard guard{value::TypeTags::Nothing, 0};
    auto obj = runAndGetObject(compiledExpr.get(), guard);
    ASSERT_EQUALS(0u, obj->size());
}

TEST_F(SBENewObjTest, BuildsObjectFromNameValuePairs) {
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kNewObj,
        sbe::makeEs(makeE<EConstant>("a"),
                    makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(5)),
                    makeE<EConstant>("b"),
                    makeE<EConstant>("hello")));
    auto compiledExpr = compileExpression(*expr);

    value::ValueGuard guard{value::TypeTags::Nothing, 0};
    auto obj = runAndGetObject(compiledExpr.get(), guard);

    ASSERT_EQUALS(2u, obj->size());

    auto [aTag, aVal] = obj->getField("a");
    ASSERT_EQUALS(value::TypeTags::NumberInt64, aTag);
    ASSERT_EQUALS(5, value::bitcastTo<int64_t>(aVal));

    auto [bTag, bVal] = obj->getField("b");
    ASSERT(value::isString(bTag));
    ASSERT_EQUALS("hello", value::getStringView(bTag, bVal));
}

TEST_F(SBENewObjTest, PreservesFieldNamesLongerThanTheSmallStringLimit) {
    const std::string longName = "longishNameForNestedDocument";
    ASSERT_GT(longName.size(), 15u);

    auto expr =
        sbe::makeE<sbe::EFunction>(EFn::kNewObj,
                                   sbe::makeEs(makeE<EConstant>(longName),
                                               makeE<EConstant>(value::TypeTags::NumberInt32,
                                                                value::bitcastFrom<int32_t>(7))));
    auto compiledExpr = compileExpression(*expr);

    value::ValueGuard guard{value::TypeTags::Nothing, 0};
    auto obj = runAndGetObject(compiledExpr.get(), guard);

    ASSERT_EQUALS(1u, obj->size());
    ASSERT_EQUALS(longName, obj->field(0));

    auto [tag, val] = obj->getField(longName);
    ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
    ASSERT_EQUALS(7, value::bitcastTo<int32_t>(val));
}

// A 'Nothing' value is omitted from the resulting object rather than stored.
TEST_F(SBENewObjTest, OmitsFieldsWithNothingValues) {
    auto expr =
        sbe::makeE<sbe::EFunction>(EFn::kNewObj,
                                   sbe::makeEs(makeE<EConstant>("a"),
                                               makeE<EConstant>(value::TypeTags::Nothing, 0),
                                               makeE<EConstant>("b"),
                                               makeE<EConstant>(value::TypeTags::NumberInt32,
                                                                value::bitcastFrom<int32_t>(1))));
    auto compiledExpr = compileExpression(*expr);

    value::ValueGuard guard{value::TypeTags::Nothing, 0};
    auto obj = runAndGetObject(compiledExpr.get(), guard);

    ASSERT_EQUALS(1u, obj->size());
    ASSERT_EQUALS("b", obj->field(0));
}

TEST_F(SBENewObjTest, ReturnsNothingWhenFirstFieldNameIsNotAString) {
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kNewObj,
        sbe::makeEs(
            makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)),
            makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2))));
    auto compiledExpr = compileExpression(*expr);

    runAndAssertNothing(compiledExpr.get());
}

// A bad field name may be encountered after earlier fields have already been added. The whole call
// must still evaluate to Nothing.
TEST_F(SBENewObjTest, ReturnsNothingWhenALaterFieldNameIsNotAString) {
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kNewObj,
        sbe::makeEs(
            makeE<EConstant>("a"),
            makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)),
            makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)),
            makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3))));
    auto compiledExpr = compileExpression(*expr);

    runAndAssertNothing(compiledExpr.get());
}

// Nested values are deep copied out of the input, so the result must stay valid and unchanged after
// the input slot is overwritten.
TEST_F(SBENewObjTest, DeepCopiesNestedObjectArgument) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kNewObj, sbe::makeEs(makeE<EConstant>("nested"), makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*expr);

    auto [innerTag, innerVal] = value::makeNewObject();
    value::getObjectView(innerVal)->push_back_raw(
        "inner", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));
    inputAccessor.reset(innerTag, innerVal);

    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(tag, val);
    ASSERT_EQUALS(value::TypeTags::Object, tag);

    auto [nestedTag, nestedVal] = value::getObjectView(val)->getField("nested");
    ASSERT_EQUALS(value::TypeTags::Object, nestedTag);
    // The copy must be a distinct object, not a view of the input.
    ASSERT_NOT_EQUALS(innerVal, nestedVal);

    // Releasing the input must leave the result intact.
    inputAccessor.reset();

    auto [innerFieldTag, innerFieldVal] = value::getObjectView(nestedVal)->getField("inner");
    ASSERT_EQUALS(value::TypeTags::NumberInt32, innerFieldTag);
    ASSERT_EQUALS(42, value::bitcastTo<int32_t>(innerFieldVal));
}
}  // namespace mongo::sbe
