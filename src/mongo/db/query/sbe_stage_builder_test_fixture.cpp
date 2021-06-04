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

std::tuple<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
SbeStageBuilderTestFixture::buildPlanStage(
    std::unique_ptr<QuerySolution> querySolution,
    bool hasRecordId,
    std::unique_ptr<ShardFiltererFactoryInterface> shardFiltererInterface) {
    auto findCommand = std::make_unique<FindCommandRequest>(_nss);
    const boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContextForTest(_nss));
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx(), std::move(findCommand), false, expCtx);
    ASSERT_OK(statusWithCQ.getStatus());

    stage_builder::SlotBasedStageBuilder builder{opCtx(),
                                                 CollectionPtr::null,
                                                 *statusWithCQ.getValue(),
                                                 *querySolution,
                                                 nullptr /* YieldPolicy */,
                                                 shardFiltererInterface.get()};

    auto stage = builder.build(querySolution->root());
    auto data = builder.getPlanStageData();

    auto slots = sbe::makeSV();
    if (hasRecordId) {
        slots.push_back(data.outputs.get(stage_builder::PlanStageSlots::kRecordId));
    }
    slots.push_back(data.outputs.get(stage_builder::PlanStageSlots::kResult));

    return {slots, std::move(stage), std::move(data)};
}

}  // namespace mongo
