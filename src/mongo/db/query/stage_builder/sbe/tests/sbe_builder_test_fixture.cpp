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

#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"

#include "mongo/base/string_data.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/unittest/unittest.h"

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
SbeStageBuilderTestFixture::buildPlanStage(std::unique_ptr<QuerySolution> querySolution,
                                           MultipleCollectionAccessor& colls,
                                           bool hasRecordId,
                                           BuildPlanStageParam param) {
    auto findCommand = param.makeFindCmdReq(_nss);

    boost::intrusive_ptr<ExpressionContext> expCtx = param.expCtx
        ? *param.expCtx
        : new ExpressionContextForTest(operationContext(), _nss, std::move(param.collator));

    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    if (hasRecordId) {
        // Force the builder to generate the RecordId output even if it isn't needed by the plan.
        cq->setForceGenerateRecordId(true);
    }

    CollectionMock coll(_nss);
    // TODO(SERVER-103403): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
    CollectionPtr collPtr = CollectionPtr::CollectionPtr_UNSAFE(&coll);
    if (param.shardFilterInterface) {
        auto shardFilterer = param.shardFilterInterface->makeShardFilterer(operationContext());
        collPtr.setShardKeyPattern(shardFilterer->getKeyPattern().toBSON());
        colls = MultipleCollectionAccessor(
            shard_role_mock::acquireCollectionMocked(operationContext(), _nss, std::move(collPtr)));
    }

    stage_builder::SlotBasedStageBuilder builder{
        operationContext(), colls, *cq, *querySolution, getYieldPolicy()};

    auto [stage, data] = builder.build(querySolution->root());

    // Reset "shardFilterer".
    if (auto shardFiltererSlot = data.env->getSlotIfExists("shardFilterer"_sd);
        shardFiltererSlot && param.shardFilterInterface) {
        auto shardFilterer = param.shardFilterInterface->makeShardFilterer(operationContext());
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

void GoldenSbeStageBuilderTestFixture::createCollection(const std::vector<BSONObj>& docs,
                                                        boost::optional<BSONObj> indexKeyPattern,
                                                        CollectionOptions options) {
    ASSERT_FALSE(_collInitialized) << "collection has been initialized";
    _collInitialized = true;
    // Create collection and index
    ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, CollectionOptions()));
    if (indexKeyPattern) {
        ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
            operationContext(),
            _nss,
            {BSON("v" << 2 << "name" << DBClientBase::genIndexName(*indexKeyPattern) << "key"
                      << *indexKeyPattern)}));
    }
    insertDocuments(docs);
}

void GoldenSbeStageBuilderTestFixture::runTest(std::unique_ptr<QuerySolutionNode> root,
                                               const mongo::BSONArray& expectedValue,
                                               BuildPlanStageParam param) {
    auto querySolution = makeQuerySolution(std::move(root));

    // Translate the QuerySolution tree to an sbe::PlanStage.
    boost::optional<CollectionOrViewAcquisition> localColl;
    MultipleCollectionAccessor colls;
    if (_collInitialized) {
        localColl.emplace(
            acquireCollectionOrView(operationContext(),
                                    CollectionOrViewAcquisitionRequest::fromOpCtx(
                                        operationContext(), _nss, AcquisitionPrerequisites::kRead),
                                    MODE_IS));
        colls = MultipleCollectionAccessor(*localColl);
    }

    auto [resultSlots, stage, data, _] =
        buildPlanStage(std::move(querySolution), colls, false /*hasRecordId*/, std::move(param));
    auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots);
    ASSERT_EQ(resultAccessors.size(), 1u);

    // Print the stage explain output and verify.
    _gctx->printTestHeader(GoldenTestContext::HeaderFormat::Text);
    auto explain = sbe::DebugPrinter().print(*stage.get());
    _gctx->outStream() << (localColl ? replaceUuid(explain, localColl->getCollection().uuid())
                                     : explain);
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
    _planStageData = std::make_unique<stage_builder::PlanStageStaticData>();
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
