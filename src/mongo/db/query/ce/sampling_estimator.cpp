/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/ce/sampling_estimator.h"

#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/cost_based_ranker/estimates.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/platform/basic.h"

namespace mongo::ce {
std::unique_ptr<CanonicalQuery> SamplingEstimator::makeCanonicalQuery(const NamespaceString& nss,
                                                                      OperationContext* opCtx,
                                                                      size_t sampleSize) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(nss));
    findCommand->setLimit(sampleSize);

    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build();

    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx,
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    return std::move(statusWithCQ.getValue());
}

/*
 * The sample size is calculated based on the confidence level and margin of error required.
 */
size_t SamplingEstimator::calculateSampleSize() {
    // TODO SERVER-94063: Calculate the sample size.
    return 500;
}

std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
SamplingEstimator::generateRandomSamplingPlan(PlanYieldPolicy* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();
    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            collection->ns().dbName(),
                                            staticData->resultSlot,
                                            boost::none /* recordIdSlot */,
                                            boost::none /* snapshotIdSlot */,
                                            boost::none /* indexIdentSlot */,
                                            boost::none /* indexKeySlot */,
                                            boost::none /* keyPatternSlot */,
                                            boost::none /* oplogTsSlot */,
                                            std::vector<std::string>{} /* scanFieldNames */,
                                            sbe::value::SlotVector{} /* scanFieldSlots */,
                                            boost::none /* seekRecordIdSlot */,
                                            boost::none /* minRecordIdSlot */,
                                            boost::none /* maxRecordIdSlot */,
                                            true /* forward */,
                                            sbeYieldPolicy,
                                            0 /* nodeId */,
                                            sbe::ScanCallbacks{},
                                            false /* lowPriority */,
                                            true /* useRandomCursor */);

    stage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(stage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(_sampleSize)),
        nullptr /* skip */,
        0 /* nodeId */);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(stage), std::move(data)};
}

void SamplingEstimator::generateRandomSample(size_t sampleSize) {
    // Create a CanonicalQuery for the sampling plan.
    auto cq = makeCanonicalQuery(_collections.getMainCollection()->ns(), _opCtx, sampleSize);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(
        _opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, _collections, cq->nss());

    auto plan = generateRandomSamplingPlan(sbeYieldPolicy.get());

    // Prepare the SBE plan for execution.
    prepareSlotBasedExecutableTree(_opCtx,
                                   plan.first.get(),
                                   &plan.second,
                                   *cq,
                                   _collections,
                                   sbeYieldPolicy.get(),
                                   false /* preparingFromCache */);

    // Create a PlanExecutor for the execution of the sampling plan.
    auto exec = std::move(mongo::plan_executor_factory::make(_opCtx,
                                                             std::move(cq),
                                                             nullptr /*solution*/,
                                                             std::move(plan),
                                                             QueryPlannerParams::DEFAULT,
                                                             _collections.getMainCollection()->ns(),
                                                             std::move(sbeYieldPolicy),
                                                             false /* isFromPlanCache */,
                                                             false /* cachedPlanHash */)
                              .getValue());

    // This function call could be a re-sample request, so the previous sample should be cleared.
    _sample.clear();
    BSONObj obj;
    // Execute the plan, exhaust results and cache the sample.
    while (PlanExecutor::ADVANCED == exec->getNext(&obj, nullptr)) {
        _sample.push_back(obj.getOwned());
    }
    return;
}

void SamplingEstimator::generateRandomSample() {
    generateRandomSample(_sampleSize);
    return;
}

void SamplingEstimator::generateChunkSample(size_t sampleSize) {
    // TODO SERVER-93729: Implement chunk-based sampling CE approach.
    return;
}

void SamplingEstimator::generateChunkSample() {
    generateChunkSample(_sampleSize);
    return;
}

CardinalityEstimate SamplingEstimator::estimateCardinality(const MatchExpression* expr) {
    size_t cnt = 0;
    for (const auto& doc : _sample) {
        if (expr->matchesBSON(doc, nullptr)) {
            cnt++;
        }
    }
    double estimate = (cnt * getCollCard()) / _sampleSize;
    CardinalityEstimate ce(mongo::cost_based_ranker::CardinalityType{estimate},
                           mongo::cost_based_ranker::EstimationSource::Sampling);
    return ce;
}

std::vector<CardinalityEstimate> SamplingEstimator::estimateCardinality(
    const std::vector<MatchExpression*>& expressions) {
    std::vector<CardinalityEstimate> estimates;
    for (auto& expr : expressions) {
        estimates.push_back(estimateCardinality(expr));
    }

    return estimates;
}

SamplingEstimator::SamplingEstimator(OperationContext* opCtx,
                                     const MultipleCollectionAccessor& collections,
                                     size_t sampleSize,
                                     SamplingStyle samplingStyle,
                                     CardinalityEstimate collectionCard)
    : _opCtx(opCtx),
      _collections(collections),
      _sampleSize(sampleSize),
      _collectionCard(collectionCard) {

    if (samplingStyle == SamplingStyle::kRandom) {
        generateRandomSample();
    } else {
        generateChunkSample();
    }
}

SamplingEstimator::SamplingEstimator(OperationContext* opCtx,
                                     const MultipleCollectionAccessor& collections,
                                     SamplingStyle samplingStyle,
                                     CardinalityEstimate collectionCard)
    : SamplingEstimator(opCtx, collections, calculateSampleSize(), samplingStyle, collectionCard) {}

SamplingEstimator::~SamplingEstimator() {}

}  // namespace mongo::ce
