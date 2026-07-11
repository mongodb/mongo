// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

namespace mongo::sbe {
using SBERankTest = EExpressionTestFixture;

TEST_F(SBERankTest, ComputeRank) {
    value::OwnedValueAccessor rankAccessor;
    value::OwnedValueAccessor denseRankAccessor;
    value::OwnedValueAccessor argAccessor;
    auto rankSlot = bindAccessor(&rankAccessor);
    auto denseRankSlot = bindAccessor(&denseRankAccessor);
    auto argSlot = bindAccessor(&argAccessor);
    auto rankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggRank,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledRankExpr = compileAggExpression(*rankExpr, &rankAccessor);
    auto denseRankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggDenseRank,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledDenseRankExpr = compileAggExpression(*denseRankExpr, &denseRankAccessor);
    auto finalRankExpr =
        sbe::makeE<sbe::EFunction>(EFn::kAggRankFinalize, sbe::makeEs(makeE<EVariable>(rankSlot)));
    auto finalCompiledRankExpr = compileExpression(*finalRankExpr);
    auto finalDenseRankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggRankFinalize, sbe::makeEs(makeE<EVariable>(denseRankSlot)));
    auto finalCompiledDenseRankExpr = compileExpression(*finalDenseRankExpr);

    std::vector<int> values{100, 200, 200, 200, 300};
    std::vector<int> ranks{1, 2, 2, 2, 5};
    std::vector<int> denseRanks{1, 2, 2, 2, 3};
    for (size_t i = 0; i < values.size(); i++) {
        argAccessor.reset(value::TypeTags::NumberInt32, values[i]);
        auto [tag, val] = runCompiledExpression(compiledRankExpr.get());
        rankAccessor.reset(tag, val);
        std::tie(tag, val) = runCompiledExpression(compiledDenseRankExpr.get());
        denseRankAccessor.reset(tag, val);

        std::tie(tag, val) = runCompiledExpression(finalCompiledRankExpr.get());
        ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(val), ranks[i]);

        std::tie(tag, val) = runCompiledExpression(finalCompiledDenseRankExpr.get());
        ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(val), denseRanks[i]);
    }
}

TEST_F(SBERankTest, ComputeRankBeyond32Bit) {
    value::OwnedValueAccessor rankAccessor;
    value::OwnedValueAccessor denseRankAccessor;
    value::OwnedValueAccessor argAccessor;
    auto rankSlot = bindAccessor(&rankAccessor);
    auto denseRankSlot = bindAccessor(&denseRankAccessor);
    auto argSlot = bindAccessor(&argAccessor);
    auto rankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggRank,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledRankExpr = compileAggExpression(*rankExpr, &rankAccessor);
    auto denseRankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggDenseRank,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledDenseRankExpr = compileAggExpression(*denseRankExpr, &denseRankAccessor);
    auto finalRankExpr =
        sbe::makeE<sbe::EFunction>(EFn::kAggRankFinalize, sbe::makeEs(makeE<EVariable>(rankSlot)));
    auto finalCompiledRankExpr = compileExpression(*finalRankExpr);
    auto finalDenseRankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggRankFinalize, sbe::makeEs(makeE<EVariable>(denseRankSlot)));
    auto finalCompiledDenseRankExpr = compileExpression(*finalDenseRankExpr);

    auto setInt32MaxRankState = [](value::OwnedValueAccessor& accessor) {
        auto [newStateTag, newStateVal] = value::makeNewArray();
        auto newState = value::getArrayView(newStateVal);
        newState->push_back_raw(value::TypeTags::NumberInt32, 0);  // kLastValue
        newState->push_back_raw(value::TypeTags::Boolean,
                                value::bitcastFrom<bool>(false));  // kLastValueIsNothing
        newState->push_back_raw(value::TypeTags::NumberInt64,
                                std::numeric_limits<int32_t>::max());  // kLastRank
        newState->push_back_raw(value::TypeTags::NumberInt64, 1);      // kSameRankCount
        auto sortSpec = std::make_unique<SortSpec>(BSON("sortKey" << 1));
        newState->push_back_raw(value::TypeTags::sortSpec,
                                value::bitcastFrom<SortSpec*>(sortSpec.release()));  // kSortSpec
        accessor.reset(newStateTag, newStateVal);
    };
    setInt32MaxRankState(rankAccessor);
    setInt32MaxRankState(denseRankAccessor);

    // int32 max - rank should return NumberInt32
    argAccessor.reset(value::TypeTags::NumberInt32, 0);
    auto [tag, val] = runCompiledExpression(compiledRankExpr.get());
    rankAccessor.reset(tag, val);
    std::tie(tag, val) = runCompiledExpression(compiledDenseRankExpr.get());
    denseRankAccessor.reset(tag, val);

    std::tie(tag, val) = runCompiledExpression(finalCompiledRankExpr.get());
    ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
    ASSERT_EQUALS(value::bitcastTo<int32_t>(val), std::numeric_limits<int32_t>::max());

    std::tie(tag, val) = runCompiledExpression(finalCompiledDenseRankExpr.get());
    ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
    ASSERT_EQUALS(value::bitcastTo<int32_t>(val), std::numeric_limits<int32_t>::max());

    // int32 max + 1 - rank should return NumberInt64
    argAccessor.reset(value::TypeTags::NumberInt32, 1);
    std::tie(tag, val) = runCompiledExpression(compiledRankExpr.get());
    rankAccessor.reset(tag, val);
    std::tie(tag, val) = runCompiledExpression(compiledDenseRankExpr.get());
    denseRankAccessor.reset(tag, val);

    std::tie(tag, val) = runCompiledExpression(finalCompiledRankExpr.get());
    ASSERT_EQUALS(value::TypeTags::NumberInt64, tag);
    ASSERT_EQUALS(value::bitcastTo<int64_t>(val), std::numeric_limits<int32_t>::max() + 2ll);

    std::tie(tag, val) = runCompiledExpression(finalCompiledDenseRankExpr.get());
    ASSERT_EQUALS(value::TypeTags::NumberInt64, tag);
    ASSERT_EQUALS(value::bitcastTo<int64_t>(val), std::numeric_limits<int32_t>::max() + 1ll);
}

TEST_F(SBERankTest, ComputeRankWithCollation) {
    value::OwnedValueAccessor rankAccessor;
    value::OwnedValueAccessor denseRankAccessor;
    value::OwnedValueAccessor argAccessor;
    value::ViewOfValueAccessor collatorAccessor;
    auto rankSlot = bindAccessor(&rankAccessor);
    auto denseRankSlot = bindAccessor(&denseRankAccessor);
    auto argSlot = bindAccessor(&argAccessor);
    auto collatorSlot = bindAccessor(&collatorAccessor);

    auto rankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggRankColl,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)),
            makeE<EVariable>(collatorSlot)));
    auto compiledRankExpr = compileAggExpression(*rankExpr, &rankAccessor);
    auto denseRankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggDenseRankColl,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)),
            makeE<EVariable>(collatorSlot)));
    auto compiledDenseRankExpr = compileAggExpression(*denseRankExpr, &denseRankAccessor);
    auto finalRankExpr =
        sbe::makeE<sbe::EFunction>(EFn::kAggRankFinalize, sbe::makeEs(makeE<EVariable>(rankSlot)));
    auto finalCompiledRankExpr = compileExpression(*finalRankExpr);
    auto finalDenseRankExpr = sbe::makeE<sbe::EFunction>(
        EFn::kAggRankFinalize, sbe::makeEs(makeE<EVariable>(denseRankSlot)));
    auto finalCompiledDenseRankExpr = compileExpression(*finalDenseRankExpr);

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterfaceMock*>(collator.get()));

    std::vector<std::string_view> values{"aaa", "bbb", "BBB", "bBb", "ccc"};
    std::vector<int> ranks{1, 2, 2, 2, 5};
    std::vector<int> denseRanks{1, 2, 2, 2, 3};
    for (size_t i = 0; i < values.size(); i++) {
        auto [tag, val] = value::makeSmallString(values[i]);
        argAccessor.reset(tag, val);
        std::tie(tag, val) = runCompiledExpression(compiledRankExpr.get());
        rankAccessor.reset(tag, val);
        std::tie(tag, val) = runCompiledExpression(compiledDenseRankExpr.get());
        denseRankAccessor.reset(tag, val);

        std::tie(tag, val) = runCompiledExpression(finalCompiledRankExpr.get());
        ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(val), ranks[i]);

        std::tie(tag, val) = runCompiledExpression(finalCompiledDenseRankExpr.get());
        ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(val), denseRanks[i]);
    }
}
}  // namespace mongo::sbe
