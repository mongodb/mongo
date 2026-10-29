// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::LimitSkipStage.
 */

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
        value::TagValueOwned inputArray = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto inputView = value::getArrayView(inputArray.value());
        for (long long i = 0; i < kValueCount; ++i) {
            inputView->push_back_raw(value::TypeTags::NumberInt64,
                                     value::bitcastFrom<long long>(i));
        }

        // Make a "limit <limitValue> skip <skipValue>" stage.
        auto [scanSlot, scanStage] = generateVirtualScan(std::move(inputArray));
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
            ctx->getRuntimeEnvAccessor(limitSlot)->reset(value::TagValueView{
                value::TypeTags::NumberInt64, value::bitcastFrom<long long>(limitValue)});
            ctx->getRuntimeEnvAccessor(skipSlot)->reset(value::TagValueView{
                value::TypeTags::NumberInt64, value::bitcastFrom<long long>(skipValue)});

            limit->open(true);
            verifyLimitSkipResult(limit.get(), resultAccessor, limitValue, skipValue);
            limit->close();
        }
    }
}

}  // namespace mongo::sbe
