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

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

unittest::GoldenTestConfig goldenTestConfigSbe{"src/mongo/db/test_output/query/stage_builder/sbe"};

std::unique_ptr<QuerySolution> SbeStageBuilderTestFixture::makeQuerySolution(
    std::unique_ptr<QuerySolutionNode> root) {
    auto querySoln = std::make_unique<QuerySolution>();
    querySoln->setRoot(std::move(root));
    return querySoln;
}

std::tuple<sbe::value::SlotVector,
           std::unique_ptr<sbe::PlanStage>,
           stage_builder::PlanStageData,
           boost::intrusive_ptr<ExpressionContext>>
SbeStageBuilderTestFixture::buildPlanStage(
    std::unique_ptr<QuerySolution> querySolution,
    MultipleCollectionAccessor& colls,
    bool hasRecordId,
    std::unique_ptr<ShardFiltererFactoryInterface> shardFiltererInterface,
    std::unique_ptr<CollatorInterface> collator) {
    auto findCommand = std::make_unique<FindCommandRequest>(_nss);
    const boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContextForTest(operationContext(), _nss, std::move(collator)));
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    if (hasRecordId) {
        // Force the builder to generate the RecordId output even if it isn't needed by the plan.
        cq->setForceGenerateRecordId(true);
    }

    CollectionMock coll(_nss);
    CollectionPtr collPtr(&coll);
    if (shardFiltererInterface) {
        auto shardFilterer = shardFiltererInterface->makeShardFilterer(operationContext());
        collPtr.setShardKeyPattern(shardFilterer->getKeyPattern().toBSON());
        colls = MultipleCollectionAccessor(collPtr);
    }

    stage_builder::SlotBasedStageBuilder builder{
        operationContext(), colls, *cq, *querySolution, getYieldPolicy()};

    auto [stage, data] = builder.build(querySolution->root());

    // Reset "shardFilterer".
    if (auto shardFiltererSlot = data.env->getSlotIfExists("shardFilterer"_sd);
        shardFiltererSlot && shardFiltererInterface) {
        auto shardFilterer = shardFiltererInterface->makeShardFilterer(operationContext());
        data.env->resetSlot(*shardFiltererSlot,
                            sbe::value::TypeTags::shardFilterer,
                            sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release()),
                            true);
    }

    auto slots = sbe::makeSV();
    if (hasRecordId) {
        slots.push_back(*data.staticData->recordIdSlot);
    }
    slots.push_back(*data.staticData->resultSlot);

    // 'expCtx' owns the collator and a collator slot is registered into the runtime environment
    // while creating 'builder'. So, the caller should retain the 'expCtx' until the execution is
    // done. Otherwise, the collator slot becomes invalid at runtime because the canonical query
    // which is created here owns the 'expCtx' which is deleted with the canonical query at the end
    // of this function.
    return {slots, std::move(stage), std::move(data), expCtx};
}

void GoldenSbeStageBuilderTestFixture::runTest(std::unique_ptr<QuerySolutionNode> root,
                                               const mongo::BSONArray& expectedValue) {
    auto querySolution = makeQuerySolution(std::move(root));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    AutoGetCollection localColl(operationContext(), _nss, LockMode::MODE_IS);
    MultipleCollectionAccessor colls(*localColl);
    auto [resultSlots, stage, data, _] = buildPlanStage(std::move(querySolution),
                                                        colls,
                                                        false /*hasRecordId*/,
                                                        nullptr /*shard filterer*/,
                                                        nullptr /*collator*/);
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    // Print the stage explain output and verify.
    _gctx->printTestHeader(GoldenTestContext::HeaderFormat::Text);
    _gctx->outStream() << replaceUuid(sbe::DebugPrinter().print(*stage.get()), localColl->uuid());
    _gctx->outStream() << std::endl;
    _gctx->verifyOutput();

    // Execute the plan to verify explain output is correct.
    auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessors[0]);
    sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
    sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

    ASSERT_TRUE(PlanStageTestFixture::valueEquals(resultsTag, resultsVal, expectedTag, expectedVal))
        << "expected: " << std::make_pair(expectedTag, expectedVal)
        << " but got: " << std::make_pair(resultsTag, resultsVal);
}

std::string GoldenSbeStageBuilderTestFixture::replaceUuid(std::string input, UUID uuid) {
    auto uuidStr = uuid.toString();
    std::string replace_by = "UUID";
    // Find the first occurrence of the uuid
    size_t pos = input.find(uuidStr);

    // Iterate through the string and replace all occurrences
    while (pos != std::string::npos) {
        // Replace the uuid with the constant string
        input.replace(pos, uuidStr.size(), replace_by);

        // Find the next occurrence of the uuid
        pos = input.find(uuidStr, pos + replace_by.size());
    }

    return input;
}

void GoldenSbeStageBuilderTestFixture::insertDocuments(const std::vector<BSONObj>& docs) {
    std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

    AutoGetCollection agc(operationContext(), _nss, LockMode::MODE_IX);
    {
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(collection_internal::insertDocuments(
            operationContext(), *agc, inserts.begin(), inserts.end(), nullptr /* opDebug */));
        wuow.commit();
    }
}

void GoldenSbeExprBuilderTestFixture::setUp() {
    SbeStageBuilderTestFixture::setUp();
    _expCtx = new ExpressionContextForTest();
    _state.emplace(operationContext(),
                   _env,
                   _planStageData.get(),
                   _variables,
                   nullptr /* yieldPolicy */,
                   &_slotIdGenerator,
                   &_frameIdGenerator,
                   &_spoolIdGenerator,
                   &_inListsMap,
                   &_collatorsMap,
                   &_sortSpecMap,
                   _expCtx,
                   false /* needsMerge */,
                   false /* allowDiskUse */
    );
}

void GoldenSbeExprBuilderTestFixture::runTest(stage_builder::SbExpr sbExpr,
                                              sbe::value::TypeTags expectedTag,
                                              sbe::value::Value expectedVal,
                                              StringData test) {
    auto sbeEExpr = sbExpr.lower(*_state);
    // Print the stage explain output and verify.
    _gctx->printTestHeader(GoldenTestContext::HeaderFormat::Text);
    _gctx->outStream() << test << std::endl;
    _gctx->outStream() << sbe::DebugPrinter().print(sbeEExpr->debugPrint());
    _gctx->outStream() << std::endl;

    sbe::CompileCtx _compileCtx(std::make_unique<sbe::RuntimeEnvironment>());
    sbe::vm::CodeFragment code = sbeEExpr->compileDirect(_env.ctx);
    sbe::vm::ByteCode vm;
    auto [owned, resultsTag, resultsVal] = vm.run(&code);
    sbe::value::ValueGuard resultGuard{owned, resultsTag, resultsVal};


    ASSERT_TRUE(PlanStageTestFixture::valueEquals(resultsTag, resultsVal, expectedTag, expectedVal))
        << "for test: " << test << " expected: " << std::make_pair(expectedTag, expectedVal)
        << " but got: " << std::make_pair(resultsTag, resultsVal);
}
}  // namespace mongo
