// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::SearchCursorStage.
 */

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/stages/search_cursor.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

using SearchCursorStageTest = PlanStageTestFixture;

const BSONArray resultArray = BSON_ARRAY(fromjson(R"(
{
    "_id" : 0,
    "metaA" : 0,
    "metaB" : 1,
    "fieldA" : 200,
    "fieldB" : 300
})") << fromjson(R"(
{
    "_id" : 1,
    "metaA" : 2,
    "metaB" : 3,
    "fieldA" : 4,
    "fieldB" : 5
})"));

const BSONObj query = fromjson(R"(
{
})");

const BSONObj queryStoredSource = fromjson(R"(
{
    "returnStoredSource": true
})");

const BSONArray resultStoredSource = BSON_ARRAY(fromjson(R"(
{
    "storedSource" : {
        "fieldA" : 200,
        "fieldB" : 300
    },
    "metaA" : 0,
    "metaB" : 1
})") << fromjson(R"(
{
    "storedSource" : {
        "fieldA" : 4,
        "fieldB" : 5
    },
    "metaA" : 2,
    "metaB" : 3
})"));

std::unique_ptr<executor::TaskExecutorCursor> mockTaskExecutorCursor(OperationContext* opCtx,
                                                                     CursorId cursorId,
                                                                     const BSONArray& firstBatch) {
    auto networkInterface = std::make_unique<executor::NetworkInterfaceMock>();
    auto testExecutor = executor::makeThreadPoolTestExecutor(std::move(networkInterface));
    executor::RemoteCommandRequest req = executor::RemoteCommandRequest();
    req.opCtx = opCtx;

    std::vector<BSONObj> batchVec;
    for (const auto& ele : firstBatch) {
        batchVec.push_back(ele.Obj());
    }

    executor::TaskExecutorCursorOptions opts(/*pinConnection*/ gPinTaskExecCursorConns.load(),
                                             /*batchSize*/ boost::none,
                                             /*preFetchNextBatch*/ false);
    // When pinning is enabled, TaskExecutorCursor requires both executor and underlying executor;
    // tests use the same executor for both since there is no real connection to pin.
    return std::make_unique<executor::TaskExecutorCursor>(
        testExecutor,
        testExecutor /* underlyingExec: same as executor in tests */,
        CursorResponse{NamespaceString::kEmpty, cursorId, batchVec},
        req,
        std::move(opts));
}

std::unique_ptr<PlanStage> makeSearchCursorStage(value::SlotId idSlot,
                                                 value::SlotId resultSlot,
                                                 std::vector<std::string> metadataNames,
                                                 value::SlotVector metadataSlots,
                                                 std::vector<std::string> fieldNames,
                                                 value::SlotVector fieldSlots,
                                                 bool isStoredSource,
                                                 value::SlotId limitSlot) {
    return isStoredSource
        ? SearchCursorStage::createForStoredSource(NamespaceString::kEmpty,
                                                   UUID::gen(),
                                                   resultSlot,
                                                   metadataNames,
                                                   metadataSlots,
                                                   fieldNames,
                                                   fieldSlots,
                                                   0 /* remoteCursorId */,
                                                   boost::none /* sortSpecSlot */,
                                                   limitSlot,
                                                   boost::none /* sortKeySlot */,
                                                   boost::none /* collatorSlot */,
                                                   nullptr /* yieldPolicy */,
                                                   kEmptyPlanNodeId)
        : SearchCursorStage::createForNonStoredSource(NamespaceString::kEmpty,
                                                      UUID::gen(),
                                                      idSlot,
                                                      metadataNames,
                                                      metadataSlots,
                                                      0 /* remoteCursorId */,
                                                      boost::none /* sortSpecSlot */,
                                                      limitSlot,
                                                      boost::none /* sortKeySlot */,
                                                      boost::none /* collatorSlot */,
                                                      nullptr /* yieldPolicy */,
                                                      kEmptyPlanNodeId);
}

TEST_F(SearchCursorStageTest, SearchTestOutputs) {
    auto env = std::make_unique<RuntimeEnvironment>();

    // Register and fill input slots in the runtime environment.
    auto limitSlot = env->registerSlot("limit"sv,
                                       sbe::value::TypeTags::NumberInt64,
                                       10 /* val */,
                                       true /* owned */,
                                       getSlotIdGenerator());

    // Generate slots for the outputs.
    auto idSlot = generateSlotId();
    std::vector<std::string> metadataNames = {"metaA", "metaB"};
    auto metadataSlots = generateMultipleSlotIds(2);

    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setUUID(UUID::gen());

    // Build and prepare for execution of the search cursor stage.
    auto searchCursor = makeSearchCursorStage(idSlot,
                                              0 /* resultSlot */,
                                              metadataNames,
                                              metadataSlots,
                                              {} /* fieldNames */,
                                              {} /* fieldSlots */,
                                              false /* isStoredSource */,
                                              limitSlot);

    auto ctx = makeCompileCtx(std::move(env));
    auto remoteCursors = std::make_unique<RemoteCursorMap>();
    remoteCursors->insert(
        {0, mockTaskExecutorCursor(expCtx->getOperationContext(), 0, resultArray)});
    ctx->remoteCursors = remoteCursors.get();

    prepareTree(ctx.get(), searchCursor.get());

    // Test that all of the output slots are filled correctly.
    int i = 0;
    for (auto st = searchCursor->getNext(); st == PlanState::ADVANCED;
         st = searchCursor->getNext(), i++) {
        auto curElem = resultArray[i].Obj();

        auto [idTag, idVal] = searchCursor->getAccessor(*ctx, idSlot)->getViewOfValue();
        ASSERT_EQ(idTag, sbe::value::TypeTags::NumberInt32);
        ASSERT_EQ(curElem.getIntField("_id"), value::bitcastTo<int32_t>(idVal));

        for (size_t p = 0; p < metadataNames.size(); ++p) {
            ASSERT(curElem.hasField(metadataNames[p]));
            auto [tag, val] = searchCursor->getAccessor(*ctx, metadataSlots[p])->getViewOfValue();
            auto expVal = curElem[metadataNames[p]].Int();
            ASSERT_EQ(tag, sbe::value::TypeTags::NumberInt32);
            ASSERT_EQ(value::bitcastTo<int32_t>(val), expVal);
        }
    }
}

TEST_F(SearchCursorStageTest, SearchTestLimit) {
    auto env = std::make_unique<RuntimeEnvironment>();

    // Register and fill input slots in the runtime environment.
    auto limitSlot = env->registerSlot("limit"sv,
                                       sbe::value::TypeTags::NumberInt64,
                                       1 /* val */,
                                       true /* owned */,
                                       getSlotIdGenerator());

    // Generate slots for the outputs.
    auto resultSlot = generateSlotId();
    std::vector<std::string> metadataNames = {"metaA", "metaB"};
    auto metadataSlots = generateMultipleSlotIds(2);
    std::vector<std::string> fieldNames = {"fieldA", "fieldB"};
    auto fieldSlots = generateMultipleSlotIds(2);

    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setUUID(UUID::gen());


    // Build and prepare for execution of the search cursor stage.
    auto searchCursor = makeSearchCursorStage(0 /* idSlot */,
                                              resultSlot,
                                              metadataNames,
                                              metadataSlots,
                                              fieldNames,
                                              fieldSlots,
                                              true /* isStoredSource */,
                                              limitSlot);

    auto ctx = makeCompileCtx(std::move(env));
    auto remoteCursors = std::make_unique<RemoteCursorMap>();
    remoteCursors->insert(
        {0, mockTaskExecutorCursor(expCtx->getOperationContext(), 0, resultStoredSource)});
    ctx->remoteCursors = remoteCursors.get();

    prepareTree(ctx.get(), searchCursor.get());

    // Test that the limit of the query is 1 and the second doc will not be returned.
    int i = 0;
    for (auto st = searchCursor->getNext(); st == PlanState::ADVANCED;
         st = searchCursor->getNext(), i++) {
    }
    ASSERT_EQ(i, 1);
}

TEST_F(SearchCursorStageTest, SearchTestStoredSource) {
    auto env = std::make_unique<RuntimeEnvironment>();

    // Register and fill input slots in the runtime environment.
    auto limitSlot = env->registerSlot("limit"sv,
                                       sbe::value::TypeTags::NumberInt64,
                                       10 /* val */,
                                       true /* owned */,
                                       getSlotIdGenerator());

    // Generate slots for the outputs.
    auto resultSlot = generateSlotId();
    std::vector<std::string> metadataNames = {"metaA", "metaB"};
    auto metadataSlots = generateMultipleSlotIds(2);
    std::vector<std::string> fieldNames = {"fieldA", "fieldB"};
    auto fieldSlots = generateMultipleSlotIds(2);

    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setUUID(UUID::gen());

    // Build and prepare for execution of the search cursor stage.
    auto searchCursor = makeSearchCursorStage(0 /* idSlot */,
                                              resultSlot,
                                              metadataNames,
                                              metadataSlots,
                                              fieldNames,
                                              fieldSlots,
                                              true /* isStoredSource */,
                                              limitSlot);

    auto ctx = makeCompileCtx(std::move(env));
    auto remoteCursors = std::make_unique<RemoteCursorMap>();
    remoteCursors->insert(
        {0, mockTaskExecutorCursor(expCtx->getOperationContext(), 0, resultStoredSource)});
    ctx->remoteCursors = remoteCursors.get();

    prepareTree(ctx.get(), searchCursor.get());

    // Test that all of the output slots are filled correctly.
    int i = 0;
    for (auto st = searchCursor->getNext(); st == PlanState::ADVANCED;
         st = searchCursor->getNext(), i++) {
        auto curElem = resultStoredSource[i].Obj();

        auto [actualTag, actualVal] = searchCursor->getAccessor(*ctx, resultSlot)->getViewOfValue();
        value::TagValueOwned expected =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(curElem["storedSource"].Obj()));
        ASSERT_SBE_VALUE_EQ(actualTag, actualVal, expected.tag(), expected.value());

        for (size_t p = 0; p < metadataNames.size(); ++p) {
            ASSERT(curElem.hasField(metadataNames[p]));
            auto [tag, val] = searchCursor->getAccessor(*ctx, metadataSlots[p])->getViewOfValue();
            auto expVal = curElem[metadataNames[p]].Int();
            ASSERT_EQ(tag, sbe::value::TypeTags::NumberInt32);
            ASSERT_EQ(value::bitcastTo<int32_t>(val), expVal);
        }

        for (size_t p = 0; p < fieldNames.size(); ++p) {
            ASSERT(curElem["storedSource"].Obj().hasField(fieldNames[p]));
            auto [tag, val] = searchCursor->getAccessor(*ctx, fieldSlots[p])->getViewOfValue();
            auto expVal = curElem["storedSource"][fieldNames[p]].Int();
            ASSERT_EQ(tag, sbe::value::TypeTags::NumberInt32);
            ASSERT_EQ(value::bitcastTo<int32_t>(val), expVal);
        }
    }
}
}  // namespace mongo::sbe
