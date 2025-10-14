/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::LimitSkipStage.
 */

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <utility>


namespace mongo::sbe {

class LimitSkipStageTest : public PlanStageTestFixture {
protected:
    static constexpr long long kValueCount = 1000;

    std::pair<std::unique_ptr<PlanStage>, value::SlotId> buildLimitSkipTree(
        std::unique_ptr<EExpression> limitExpr, std::unique_ptr<EExpression> skipExpr) {
        // Make an input array containing 64-integers 0 thru 999, inclusive.
        auto [inputTag, inputVal] = value::makeNewArray();
        value::ValueGuard inputGuard{inputTag, inputVal};
        auto inputView = value::getArrayView(inputVal);
        for (long long i = 0; i < kValueCount; ++i) {
            inputView->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(i));
        }

        // Make a "limit <limitValue> skip <skipValue>" stage.
        inputGuard.reset();
        auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);
        auto limit = makeS<LimitSkipStage>(
            std::move(scanStage), std::move(limitExpr), std::move(skipExpr), kEmptyPlanNodeId);
        return {std::move(limit), scanSlot};
    }

    void verifyLimitSkipResult(PlanStage* limit,
                               value::SlotAccessor* resultAccessor,
                               long long limitValue,
                               long long skipValue) {
        // Verify that `limit` produces exactly skip thru skip + limit, inclusive.
        auto resultCount = std::min(limitValue, std::max(0LL, kValueCount - skipValue));
        for (long long i = 0; i < resultCount; ++i) {
            ASSERT_TRUE(limit->getNext() == PlanState::ADVANCED)
                << "limitValue: " << limitValue << ", skipValue: " << skipValue << ", i: " << i;

            auto [tag, val] = resultAccessor->getViewOfValue();
            ASSERT_TRUE(tag == value::TypeTags::NumberInt64)
                << "limitValue: " << limitValue << ", skipValue: " << skipValue << ", i: " << i;
            ASSERT_TRUE(value::bitcastTo<long long>(val) == i + skipValue)
                << "limitValue: " << limitValue << ", skipValue: " << skipValue << ", i: " << i;
        }

        // We expect LimitSkipStage to report EOF as soon as the limit is reached, even before the
        // getNext() call that returned PlanState::IS_EOF. This allows us to close cursors as soon
        // as possible.
        if (resultCount == limitValue) {
            ASSERT_TRUE(limit->getCommonStats()->isEOF)
                << "limitValue: " << limitValue << ", skipValue: " << skipValue;
        }
        ASSERT_TRUE(limit->getNext() == PlanState::IS_EOF)
            << "limitValue: " << limitValue << ", skipValue: " << skipValue;
    }

    void runLimitSkipTest(std::unique_ptr<RuntimeEnvironment> env,
                          std::unique_ptr<EExpression> limitExpr,
                          std::unique_ptr<EExpression> skipExpr,
                          long long limitValue,
                          long long skipValue) {
        const auto& [limit, scanSlot] =
            buildLimitSkipTree(std::move(limitExpr), std::move(skipExpr));

        auto ctx = makeCompileCtx(std::move(env));
        auto resultAccessor = prepareTree(ctx.get(), limit.get(), scanSlot);

        verifyLimitSkipResult(limit.get(), resultAccessor, limitValue, skipValue);
    }
};

TEST_F(LimitSkipStageTest, LimitSimpleTest) {
    auto ctx = makeCompileCtx();

    // Make a "limit 1000" stage.
    auto limit = makeS<LimitSkipStage>(
        makeS<CoScanStage>(kEmptyPlanNodeId),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1000)),
        nullptr,
        kEmptyPlanNodeId);

    prepareTree(ctx.get(), limit.get());

    // Verify that `limit` produces at least 1000 values.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(limit->getNext() == PlanState::ADVANCED);
    }

    // Verify that `limit` does not produce more than 1000 values.
    ASSERT_TRUE(limit->getNext() == PlanState::IS_EOF);
}

TEST_F(LimitSkipStageTest, LimitSkipSimpleTest) {
    static constexpr long long kLimitValue = 200;
    static constexpr long long kSkipValue = 300;

    runLimitSkipTest(
        std::make_unique<RuntimeEnvironment>(),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(kLimitValue)),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(kSkipValue)),
        kLimitValue,
        kSkipValue);
}

TEST_F(LimitSkipStageTest, SkipMoreThanLimitTest) {
    static constexpr long long kLimitValue = 100;
    static constexpr long long kSkipValue = 950;

    runLimitSkipTest(
        std::make_unique<RuntimeEnvironment>(),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(kLimitValue)),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(kSkipValue)),
        kLimitValue,
        kSkipValue);
}

TEST_F(LimitSkipStageTest, SkipMoreThanValueCountTest) {
    static constexpr long long kLimitValue = 100;
    static constexpr long long kSkipValue = kValueCount + 100;

    runLimitSkipTest(
        std::make_unique<RuntimeEnvironment>(),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(kLimitValue)),
        makeConstant(value::TypeTags::NumberInt64, value::bitcastFrom<long long>(kSkipValue)),
        kLimitValue,
        kSkipValue);
}

TEST_F(LimitSkipStageTest, LimitSkipSlotTest) {
    static constexpr long long kLimitValue = 200;
    static constexpr long long kSkipValue = 300;

    std::unique_ptr<RuntimeEnvironment> env = std::make_unique<RuntimeEnvironment>();
    auto limitExpr = makeE<EVariable>(env->registerSlot(value::TypeTags::NumberInt64,
                                                        value::bitcastFrom<long long>(kLimitValue),
                                                        false,
                                                        getSlotIdGenerator()));
    auto skipExpr = makeE<EVariable>(env->registerSlot(value::TypeTags::NumberInt64,
                                                       value::bitcastFrom<long long>(kSkipValue),
                                                       false,
                                                       getSlotIdGenerator()));
    runLimitSkipTest(
        std::move(env), std::move(limitExpr), std::move(skipExpr), kLimitValue, kSkipValue);
}

TEST_F(LimitSkipStageTest, LimitSkipReopenTest) {
    std::unique_ptr<RuntimeEnvironment> env = std::make_unique<RuntimeEnvironment>();
    auto limitSlot = env->registerSlot(value::TypeTags::NumberInt64,
                                       value::bitcastFrom<long long>(0LL),
                                       false,
                                       getSlotIdGenerator());
    auto skipSlot = env->registerSlot(value::TypeTags::NumberInt64,
                                      value::bitcastFrom<long long>(0LL),
                                      false,
                                      getSlotIdGenerator());
    auto limitExpr = makeE<EVariable>(limitSlot);
    auto skipExpr = makeE<EVariable>(skipSlot);

    const auto& [limit, scanSlot] = buildLimitSkipTree(std::move(limitExpr), std::move(skipExpr));

    auto ctx = makeCompileCtx(std::move(env));
    auto resultAccessor = prepareTree(ctx.get(), limit.get(), scanSlot);
    limit->close();

    for (long long limitValue = 0; limitValue <= 1; ++limitValue) {
        for (long long skipValue = 0; skipValue <= 10; skipValue += 5) {
            ctx->getRuntimeEnvAccessor(limitSlot)->reset(
                false, value::TypeTags::NumberInt64, value::bitcastFrom<long long>(limitValue));
            ctx->getRuntimeEnvAccessor(skipSlot)->reset(
                false, value::TypeTags::NumberInt64, value::bitcastFrom<long long>(skipValue));

            limit->open(true);
            verifyLimitSkipResult(limit.get(), resultAccessor, limitValue, skipValue);
            limit->close();
        }
    }
}

}  // namespace mongo::sbe
