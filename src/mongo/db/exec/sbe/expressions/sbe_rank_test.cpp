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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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
        "aggRank",
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledRankExpr = compileAggExpression(*rankExpr, &rankAccessor);
    auto denseRankExpr = sbe::makeE<sbe::EFunction>(
        "aggDenseRank",
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledDenseRankExpr = compileAggExpression(*denseRankExpr, &denseRankAccessor);
    auto finalRankExpr =
        sbe::makeE<sbe::EFunction>("aggRankFinalize", sbe::makeEs(makeE<EVariable>(rankSlot)));
    auto finalCompiledRankExpr = compileExpression(*finalRankExpr);
    auto finalDenseRankExpr =
        sbe::makeE<sbe::EFunction>("aggRankFinalize", sbe::makeEs(makeE<EVariable>(denseRankSlot)));
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
        "aggRank",
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledRankExpr = compileAggExpression(*rankExpr, &rankAccessor);
    auto denseRankExpr = sbe::makeE<sbe::EFunction>(
        "aggDenseRank",
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true))));
    auto compiledDenseRankExpr = compileAggExpression(*denseRankExpr, &denseRankAccessor);
    auto finalRankExpr =
        sbe::makeE<sbe::EFunction>("aggRankFinalize", sbe::makeEs(makeE<EVariable>(rankSlot)));
    auto finalCompiledRankExpr = compileExpression(*finalRankExpr);
    auto finalDenseRankExpr =
        sbe::makeE<sbe::EFunction>("aggRankFinalize", sbe::makeEs(makeE<EVariable>(denseRankSlot)));
    auto finalCompiledDenseRankExpr = compileExpression(*finalDenseRankExpr);

    auto setInt32MaxRankState = [](value::OwnedValueAccessor& accessor) {
        auto [newStateTag, newStateVal] = value::makeNewArray();
        auto newState = value::getArrayView(newStateVal);
        newState->push_back(value::TypeTags::NumberInt32, 0);  // kLastValue
        newState->push_back(value::TypeTags::Boolean,
                            value::bitcastFrom<bool>(false));  // kLastValueIsNothing
        newState->push_back(value::TypeTags::NumberInt64,
                            std::numeric_limits<int32_t>::max());  // kLastRank
        newState->push_back(value::TypeTags::NumberInt64, 1);      // kSameRankCount
        auto sortSpec = std::make_unique<SortSpec>(BSON("sortKey" << 1));
        newState->push_back(value::TypeTags::sortSpec,
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
        "aggRankColl",
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)),
            makeE<EVariable>(collatorSlot)));
    auto compiledRankExpr = compileAggExpression(*rankExpr, &rankAccessor);
    auto denseRankExpr = sbe::makeE<sbe::EFunction>(
        "aggDenseRankColl",
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<EConstant>(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)),
            makeE<EVariable>(collatorSlot)));
    auto compiledDenseRankExpr = compileAggExpression(*denseRankExpr, &denseRankAccessor);
    auto finalRankExpr =
        sbe::makeE<sbe::EFunction>("aggRankFinalize", sbe::makeEs(makeE<EVariable>(rankSlot)));
    auto finalCompiledRankExpr = compileExpression(*finalRankExpr);
    auto finalDenseRankExpr =
        sbe::makeE<sbe::EFunction>("aggRankFinalize", sbe::makeEs(makeE<EVariable>(denseRankSlot)));
    auto finalCompiledDenseRankExpr = compileExpression(*finalDenseRankExpr);

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterfaceMock*>(collator.get()));

    std::vector<StringData> values{"aaa", "bbb", "BBB", "bBb", "ccc"};
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
