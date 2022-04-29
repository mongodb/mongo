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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"

namespace mongo {
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
    bool hasRecordId,
    std::unique_ptr<ShardFiltererFactoryInterface> shardFiltererInterface,
    std::unique_ptr<CollatorInterface> collator) {
    auto findCommand = std::make_unique<FindCommandRequest>(_nss);
    const boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContextForTest(operationContext(), _nss, std::move(collator)));
    auto statusWithCQ =
        CanonicalQuery::canonicalize(operationContext(), std::move(findCommand), false, expCtx);
    ASSERT_OK(statusWithCQ.getStatus());

    CollectionMock coll(_nss);
    CollectionPtr collPtr(&coll);
    MultipleCollectionAccessor& colls = _collections;
    if (shardFiltererInterface) {
        auto shardFilterer = shardFiltererInterface->makeShardFilterer(operationContext());
        collPtr.setShardKeyPattern(shardFilterer->getKeyPattern().toBSON());
        colls = MultipleCollectionAccessor(collPtr);
    }

    stage_builder::SlotBasedStageBuilder builder{operationContext(),
                                                 colls,
                                                 *statusWithCQ.getValue(),
                                                 *querySolution,
                                                 nullptr /* YieldPolicy */};

    auto stage = builder.build(querySolution->root());
    auto data = builder.getPlanStageData();

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
        slots.push_back(data.outputs.get(stage_builder::PlanStageSlots::kRecordId));
    }
    slots.push_back(data.outputs.get(stage_builder::PlanStageSlots::kResult));

    // 'expCtx' owns the collator and a collator slot is registered into the runtime environment
    // while creating 'builder'. So, the caller should retain the 'expCtx' until the execution is
    // done. Otherwise, the collator slot becomes invalid at runtime because the canonical query
    // which is created here owns the 'expCtx' which is deleted with the canonical query at the end
    // of this function.
    return {slots, std::move(stage), std::move(data), expCtx};
}

}  // namespace mongo
