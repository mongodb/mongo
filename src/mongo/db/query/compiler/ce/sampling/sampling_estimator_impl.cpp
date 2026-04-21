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

#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/random_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/sampling/math.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <cmath>

#include <boost/container/flat_set.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {

using CardinalityType = mongo::cost_based_ranker::CardinalityType;
using EstimationSource = mongo::cost_based_ranker::EstimationSource;

MONGO_FAIL_POINT_DEFINE(hangBeforeCBRSamplingGenerateSample);

namespace {

/**
 * Helper function to determine whether a given set of field names contains a
 * dotted path.
 */
bool containsDottedPath(const StringSet& topLevelSampleFieldNames) {
    return std::any_of(
        topLevelSampleFieldNames.begin(),
        topLevelSampleFieldNames.end(),
        [](const std::string& fieldName) { return fieldName.find('.') != std::string::npos; });
}

void validateTopLevelSampleFieldNames(const StringSet& topLevelSampleFieldNames) {
    tassert(10770100,
            "topLevelSampleFieldNames must be a non-empty set if specified.",
            !topLevelSampleFieldNames.empty());
    tassert(10670300,
            "topLevelSampleFieldNames should not contain a dotted field path",
            !containsDottedPath(topLevelSampleFieldNames));
}

/**
 * Helper function to determine if a given MatchExpression contains only fields present in the
 * sample.
 */
void checkSampleContainsMatchExpressionFields(const StringSet& topLevelSampleFieldNames,
                                              const MatchExpression* expr) {
    const auto matchExpressionFields = extractTopLevelFieldsFromMatchExpression(expr);
    for (const auto& matchField : matchExpressionFields) {
        tassert(10670301,
                "MatchExpression contains fields not present in topLevelSampleFieldNames. "
                "MatchExpression: " +
                    expr->toString(),
                topLevelSampleFieldNames.contains(matchField));
    }
}

void checkSampleContainsIndexBoundsFields(const StringSet& topLevelSampleFieldNames,
                                          const IndexBounds& bounds) {
    for (auto& oil : bounds.fields) {
        tassert(10770101,
                "Field in index bounds should be included in the set of sampled fields.",
                topLevelSampleFieldNames.contains(stage_builder::getTopLevelField(oil.name)));
    }
}

/**
 * Helper function to determine if a given set of MatchExpresions contains only fields present in
 * the sample.
 */
void checkSampleContainsMatchExpressionFields(
    const StringSet& topLevelSampleFieldNames,
    const std::vector<const MatchExpression*>& expressions) {
    for (const auto& expr : expressions) {
        checkSampleContainsMatchExpressionFields(topLevelSampleFieldNames, expr);
    }
}

/**
 * Help function mapping confidence intervals to Z-scores.
 * A "95 confidence interval" means that 95% of the observations (in our case CEs computed from
 * samples) lie within that interval. The interval is defined based on a number of standard
 * deviations(Z-score) from the mean. The Z-score for 95% interval is 1.96, meaning that 95% of the
 * observations lie within 1.96 standard deviations from the mean.
 * https://en.wikipedia.org/wiki/Standard_score
 */
double getZScore(SamplingConfidenceIntervalEnum confidenceInterval) {
    switch (confidenceInterval) {
        case SamplingConfidenceIntervalEnum::k90:
            return 1.645;
        case SamplingConfidenceIntervalEnum::k95:
            return 1.96;
        case SamplingConfidenceIntervalEnum::k99:
            return 2.576;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * This helper creates a 'sbe::ScanStage' that scans documents either by randomly seeking documents
 * or sequentially scanning documents from the target collection.
 * 'recordId'/'recordIdSlot' indicates the slot id that this scan stage produces results to. The
 * scan stage can produce the record and the recordID if provided the slot id. RecordId is useful
 * for the chunk-based sampling in order to pick starting point of each chunk.
 * 'useRandomCursor' indicates the scan method. Random sampling plan requires the random cursor to
 * perform the sampling. Sequential scan is useful to generate a repeatable sample by scanning
 * documents from the start of the target collection.
 */
std::unique_ptr<sbe::PlanStage> makeScanStage(const CollectionPtr& collection,
                                              boost::optional<sbe::value::SlotId> recordSlot,
                                              boost::optional<sbe::value::SlotId> recordIdSlot,
                                              boost::optional<sbe::value::SlotId> minRecordIdSlot,
                                              bool useRandomCursor,
                                              PlanYieldPolicySBE* sbeYieldPolicy) {
    sbe::value::SlotVector scanFieldSlots;
    std::vector<std::string> scanFieldNames;
    sbe::ScanOpenCallback scanOpenCallback{};
    if (useRandomCursor) {
        return sbe::makeS<sbe::RandomScanStage>(collection->uuid(),
                                                collection->ns().dbName(),
                                                recordSlot,
                                                recordIdSlot,
                                                boost::none /* snapshotIdSlot */,
                                                boost::none /* indexIdentSlot */,
                                                boost::none /* indexKeySlot */,
                                                boost::none /* keyPatternSlot */,
                                                scanFieldNames,
                                                scanFieldSlots,
                                                sbeYieldPolicy,
                                                0 /* nodeId */);
    } else if (minRecordIdSlot) {
        return sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                          collection->ns().dbName(),
                                          recordSlot,
                                          recordIdSlot,
                                          boost::none /* snapshotIdSlot */,
                                          boost::none /* indexIdentSlot */,
                                          boost::none /* indexKeySlot */,
                                          boost::none /* keyPatternSlot */,
                                          scanFieldNames,
                                          scanFieldSlots,
                                          minRecordIdSlot,
                                          boost::none /* maxRecordIdSlot */,
                                          true /* forward */,
                                          sbeYieldPolicy,
                                          0 /* nodeId */,
                                          std::move(scanOpenCallback));
    }
    return sbe::makeS<sbe::GenericScanStage>(collection->uuid(),
                                             collection->ns().dbName(),
                                             recordSlot,
                                             recordIdSlot,
                                             boost::none /* snapshotIdSlot */,
                                             boost::none /* indexIdentSlot */,
                                             boost::none /* indexKeySlot */,
                                             boost::none /* keyPatternSlot */,
                                             scanFieldNames,
                                             scanFieldSlots,
                                             true /* forward */,
                                             sbeYieldPolicy,
                                             0 /* nodeId */,
                                             std::move(scanOpenCallback));
}

/**
 * Tries to evaluate 'expr' against 'doc'. Returns true/false for match/no-match, or boost::none if
 * evaluation threw a DBException.
 */
boost::optional<bool> tryMatchesBSON(const MatchExpression* expr, const BSONObj& doc) {
    try {
        return exec::matcher::matchesBSON(expr, doc, nullptr);
    } catch (const DBException&) {
        return boost::none;
    }
}

/**
 * Computes a scaled cardinality estimate from the given match count and error count. The
 * selectivity is computed over only the successfully evaluated documents (i.e., the effective
 * sample size is 'sampleSize - errorCount').
 */
CardinalityEstimate makeScaledEstimate(double matchCount,
                                       size_t errorCount,
                                       size_t sampleSize,
                                       double collCard) {
    size_t effectiveSampleSize = sampleSize - errorCount;
    double estimate = effectiveSampleSize > 0 ? (matchCount * collCard) / effectiveSampleSize : 0.0;
    return CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling};
}

/**
 * This helper creates a sbe::ProjectStage which is used to apply an inclusion projection on the
 * documents when generating the sample. 'stage' is the child SBE plan stage that we use as input to
 * create the current stage. 'inputSlot' is the SBE slot where the document that we want to apply
 * projection to is stored. 'outputSlot' is the SBE slot where the document with projections applied
 * to it will be stored. 'topLevelSampleFieldNames' provide the SBE bindings of field names that
 * should be in the output document and are used to apply the inclusion projection.
 */
std::unique_ptr<sbe::PlanStage> makeProjectStage(std::unique_ptr<sbe::PlanStage> stage,
                                                 sbe::value::SlotId inputSlot,
                                                 sbe::value::SlotId outputSlot,
                                                 const StringSet& topLevelSampleFieldNames) {
    // Populate the vector for field actions with Keep for all fields since we want inclusion
    // projection.
    std::vector<sbe::MakeObjSpec::FieldAction> fieldActions;
    for (size_t i = 0; i < topLevelSampleFieldNames.size(); i++) {
        fieldActions.emplace_back(sbe::MakeObjSpec::Keep{});
    }

    std::vector<std::string> topLevelSampleFieldNamesVec{topLevelSampleFieldNames.begin(),
                                                         topLevelSampleFieldNames.end()};
    auto spec =
        std::make_unique<sbe::MakeObjSpec>(sbe::MakeObjSpec::FieldListScope::kClosed,
                                           std::move(topLevelSampleFieldNamesVec),
                                           std::move(fieldActions),
                                           sbe::MakeObjSpec::NonObjInputBehavior::kReturnNothing);
    auto specExpr =
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::makeObjSpec,
                                   sbe::value::bitcastFrom<sbe::MakeObjSpec*>(spec.release()));

    // Create a built-in SBE makeBsonObj func and pass in the 2 args: MakeObjSpec and slot to read
    // input doc from (inputSlot).
    auto func = sbe::makeE<sbe::EFunction>(
        sbe::EFn::kMakeBsonObj,
        sbe::makeEs(std::move(specExpr), sbe::makeE<sbe::EVariable>(inputSlot)));
    return sbe::makeProjectStage(std::move(stage), 0 /* nodeId */, outputSlot, std::move(func));
}
}  // namespace

StringSet extractTopLevelFieldsFromMatchExpression(const MatchExpression* expr) {
    DepsTracker deps;
    dependency_analysis::addDependencies(expr, &deps);
    StringSet topLevelFieldsSet;
    for (auto&& path : deps.fields) {
        const auto field = stage_builder::getTopLevelField(path);
        topLevelFieldsSet.emplace(std::string(field));
    }
    return topLevelFieldsSet;
}

std::unique_ptr<CanonicalQuery> SamplingEstimatorImpl::makeEmptyCanonicalQuery(
    const NamespaceString& nss, OperationContext* opCtx) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(nss));
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build();
    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx,
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    return std::move(statusWithCQ.getValue());
}


size_t SamplingEstimatorImpl::calculateSampleSize(SamplingConfidenceIntervalEnum ci,
                                                  double marginOfError) {
    uassert(9406301, "Margin of error should be larger than 0.", marginOfError > 0);
    double z = getZScore(ci);
    double ciWidth = 2 * marginOfError / 100.0;

    return static_cast<size_t>(std::lround((z * z) / (ciWidth * ciWidth)));
}

std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
SamplingEstimatorImpl::generateRandomSamplingPlan(PlanYieldPolicySBE* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.lookupCollection(_nss);
    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, true, sbeYieldPolicy);

    stage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(stage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(_sampleSize)),
        nullptr /* skip */,
        0 /* nodeId */);

    // Inject projection if topLevelSampleFieldNames is non-empty.
    if (!_topLevelSampleFieldNames.empty()) {
        auto projectResultSlot = ids.generate();
        stage = ce::makeProjectStage(std::move(stage),
                                     staticData->resultSlot.get(),
                                     projectResultSlot,
                                     _topLevelSampleFieldNames);
        staticData->resultSlot = projectResultSlot;
    }

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(stage), std::move(data)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
SamplingEstimatorImpl::generateChunkSamplingPlan(PlanYieldPolicySBE* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.lookupCollection(_nss);
    auto chunkSize = static_cast<int64_t>(_sampleSize / *_numChunks);

    boost::optional<sbe::value::SlotId> outerRid = ids.generate();
    // There's no need for the outer stage to retrieve the record/document since it'll be seeked
    // again in the inner stage.
    auto outerStage =
        makeScanStage(collection, boost::none, outerRid, boost::none, true, sbeYieldPolicy);

    // The outer stage randomly picks 'numChunks' RIds as the starting point of each of the chunks
    // forming the sample.
    outerStage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(outerStage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(*_numChunks)),
        nullptr /* skip */,
        0 /* nodeId */);

    // The inner stage seeks into the starting point picked by the outer stage and scans 'chunkSize'
    // documents sequentially.
    auto innerStage = makeScanStage(
        collection, staticData->resultSlot, boost::none, outerRid, false, sbeYieldPolicy);

    innerStage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(innerStage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(chunkSize)),
        nullptr /* skip */,
        0 /* nodeId */);


    sbe::value::SlotVector outerProjectsSlots;
    sbe::value::SlotVector outerCorrelatedSlots{*outerRid};
    auto stage = sbe::makeS<sbe::LoopJoinStage>(std::move(outerStage),
                                                std::move(innerStage),
                                                outerProjectsSlots,
                                                outerCorrelatedSlots,
                                                nullptr /* predicate */,
                                                0 /* _nodeId */);

    // Inject projection if topLevelSampleFieldNames is non-empty.
    if (!_topLevelSampleFieldNames.empty()) {
        auto projectResultSlot = ids.generate();
        stage = ce::makeProjectStage(std::move(stage),
                                     staticData->resultSlot.get(),
                                     projectResultSlot,
                                     _topLevelSampleFieldNames);
        staticData->resultSlot = projectResultSlot;
    }

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(stage), std::move(data)};
}

void SamplingEstimatorImpl::executeSamplingQueryAndSample(
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>& plan,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy) {
    sbe::DebugPrintInfo debugPrintInfo{};
    LOGV2_DEBUG(10670302,
                5,
                "SamplingCE Sampling SBE plan",
                "SBE Plan"_attr =
                    sbe::DebugPrinter{}.print(plan.first.get()->debugPrint(debugPrintInfo)));
    // Prepare the SBE plan for execution.
    prepareSlotBasedExecutableTree(_opCtx,
                                   plan.first.get(),
                                   &plan.second,
                                   *cq,
                                   _collections,
                                   sbeYieldPolicy.get(),
                                   false /* preparingFromCache */);

    // Create a PlanExecutor for the execution of the sampling plan.
    auto exec = mongo::plan_executor_factory::make(_opCtx,
                                                   std::move(cq),
                                                   nullptr /*solution*/,
                                                   std::move(plan),
                                                   _collections,
                                                   QueryPlannerParams::DEFAULT,
                                                   _nss,
                                                   std::move(sbeYieldPolicy),
                                                   false /* isFromPlanCache */,
                                                   false /* cachedPlanHash */);

    // This function call could be a re-sample request, so the previous sample should be cleared.
    _sample.clear();
    _uniqueDocCount = boost::none;
    BSONObj obj;
    // Execute the plan, exhaust results and cache the sample.
    while (PlanExecutor::ADVANCED == exec->getNext(&obj, nullptr)) {
        _sample.push_back(obj.getOwned());
    }

    _sampleSize = _sample.size();
}

void SamplingEstimatorImpl::generateFullCollScanSample() {
    // Create a CanonicalQuery for the CollScan plan.
    auto cq = makeEmptyCanonicalQuery(_nss, _opCtx);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, _nss);

    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.lookupCollection(_nss);

    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, false, sbeYieldPolicy.get());

    // Inject projection if topLevelSampleFieldNames is non-empty.
    if (!_topLevelSampleFieldNames.empty()) {
        auto projectResultSlot = ids.generate();
        stage = ce::makeProjectStage(std::move(stage),
                                     staticData->resultSlot.get(),
                                     projectResultSlot,
                                     _topLevelSampleFieldNames);
        staticData->resultSlot = projectResultSlot;
    }

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};
    auto plan =
        std::make_pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>(
            std::move(stage), std::move(data));

    executeSamplingQueryAndSample(plan, std::move(cq), std::move(sbeYieldPolicy));

    return;
}

void SamplingEstimatorImpl::generateRandomSample(size_t sampleSize) {
    // Create a CanonicalQuery for the sampling plan.
    auto cq = makeEmptyCanonicalQuery(_nss, _opCtx);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, _nss);

    auto plan = generateRandomSamplingPlan(sbeYieldPolicy.get());
    executeSamplingQueryAndSample(plan, std::move(cq), std::move(sbeYieldPolicy));

    return;
}

void SamplingEstimatorImpl::generateRandomSample() {
    generateRandomSample(_sampleSize);
    return;
}

void SamplingEstimatorImpl::generateChunkSample(size_t sampleSize) {
    // Create a CanonicalQuery for the sampling plan.
    auto cq = makeEmptyCanonicalQuery(_nss, _opCtx);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, _nss);

    auto plan = generateChunkSamplingPlan(sbeYieldPolicy.get());
    executeSamplingQueryAndSample(plan, std::move(cq), std::move(sbeYieldPolicy));

    return;
}

void SamplingEstimatorImpl::generateChunkSample() {
    generateChunkSample(_sampleSize);
    return;
}

void SamplingEstimatorImpl::generateSample(ce::ProjectionParams projectionParams) {
    _isSampleGenerated = true;
    if (auto topLevelSampleFieldNames =
            std::get_if<ce::TopLevelFieldsProjection>(&projectionParams)) {
        validateTopLevelSampleFieldNames(*topLevelSampleFieldNames);
        _topLevelSampleFieldNames = *topLevelSampleFieldNames;
    }

    // Test hook: pause here so tests can arm setYieldAllLocksHang after any multiplanning
    // trial phase is done, ensuring the yield fires inside the sampling executor.
    hangBeforeCBRSamplingGenerateSample.pauseWhileSet(_opCtx);

    if (internalQuerySamplingBySequentialScan.load()) {
        // This is only used for testing purposes when a repeatable sample is needed.
        generateSampleBySeqScanningForTesting();
    } else if (_sampleSize >= _collectionCard.cardinality().v()) {
        // If the required sample is larger than the collection, the sample is generated from all
        // the documents on the collection.
        generateFullCollScanSample();
    } else if (_samplingStyle == SamplingCEMethodEnum::kRandom) {
        generateRandomSample();
    } else {
        tassert(9372901, "The number of chunks should be positive.", _numChunks && *_numChunks > 0);
        generateChunkSample();
    }
}

void SamplingEstimatorImpl::generateSampleBySeqScanningForTesting() {
    // Create a CanonicalQuery for the sampling plan.
    auto cq = makeEmptyCanonicalQuery(_nss, _opCtx);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, _nss);

    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.lookupCollection(_nss);
    // Scan the first '_sampleSize' documents sequentially from the start of the target collection
    // in order to generate a repeatable sample.
    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, false, sbeYieldPolicy.get());
    stage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(stage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(_sampleSize)),
        nullptr /* skip */,
        0 /* nodeId */);

    // Inject projection if topLevelSampleFieldNames is non-empty.
    if (!_topLevelSampleFieldNames.empty()) {
        auto projectResultSlot = ids.generate();
        stage = ce::makeProjectStage(std::move(stage),
                                     staticData->resultSlot.get(),
                                     projectResultSlot,
                                     _topLevelSampleFieldNames);
        staticData->resultSlot = projectResultSlot;
    }

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};
    auto plan =
        std::make_pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>(
            std::move(stage), std::move(data));
    executeSamplingQueryAndSample(plan, std::move(cq), std::move(sbeYieldPolicy));

    return;
}

namespace {

/**
 * If 'expr' is an AndMatchExpression whose children are all EqualityMatchExpressions on the same
 * field path without a collator, returns {path, values}. Otherwise returns boost::none.
 *
 * This is the common shape produced when parsing $all
 */
boost::optional<std::pair<StringData, std::vector<BSONElement>>> tryExtractAllEqualities(
    const MatchExpression* expr) {
    if (expr->matchType() != MatchExpression::AND || expr->numChildren() == 0) {
        return boost::none;
    }

    // Because the CanonicalQuery normalizer sorts $and children by match type (contiguously) and
    // then by path within the same type, we can use the first and last children to check for some
    // early return conditions:
    //   - If not all children are EQ, then either/both of these children will not be EQ.
    //   - If not all children share the same path, then the first and last children will have
    //   different paths.
    //   - This optimization won't work if the common path is a dotted path.
    // Either mismatch means this $and cannot be the all-equalities fast path.
    const auto* first = expr->getChild(0);
    const auto* last = expr->getChild(expr->numChildren() - 1);
    if (first->matchType() != MatchExpression::EQ || last->matchType() != MatchExpression::EQ) {
        return boost::none;
    }
    if (first->path() != last->path()) {
        return boost::none;
    }
    if (first->path().find('.') != std::string::npos) {
        return boost::none;
    }

    // All children share the same path (verified by the check above).
    // We still need to scan every child for collation and null/undefined rejections.
    StringData commonPath = static_cast<const EqualityMatchExpression*>(first)->path();
    std::vector<BSONElement> values;
    values.reserve(expr->numChildren());

    for (size_t i = 0; i < expr->numChildren(); ++i) {
        const auto* child = expr->getChild(i);

        // Not all MatchExpressions are normalized, so we still have to double check that the
        // expression is an EQ with the same common path.
        if (child->matchType() != MatchExpression::EQ || child->path() != commonPath) {
            return boost::none;
        }

        const auto* eq = static_cast<const EqualityMatchExpression*>(child);
        // Reject collation-sensitive equalities: collator affects comparison semantics.
        if (eq->getCollator() != nullptr) {
            return boost::none;
        }
        // Reject null/undefined equalities: EqualityMatchExpression has special "null ==
        // missing" semantics for these types that our simple value comparison doesn't replicate.
        const BSONElement& rhs = eq->getData();
        if (rhs.isNull() || rhs.type() == BSONType::undefined) {
            return boost::none;
        }
        values.push_back(rhs);
    }

    return std::make_pair(commonPath, std::move(values));
}

/**
 * Returns true if 'doc' has a field at 'path' that contains ALL values in 'sortedRequiredValues'
 * (which must already be sorted by BSONElementCmpWithoutField order. Note that deduplication is not
 * necessary because $all queries are deduplicated during boolean simplification).
 *
 * For array-valued fields, uses a merge-scan to check all values are contained in the field,
 * iterating through each vector once.
 */
bool documentMatchesAllEqualities(const BSONObj& doc,
                                  StringData path,
                                  const std::vector<BSONElement>& sortedRequiredValues) {
    BSONElementCmpWithoutField cmp;

    BSONElement fieldElem = doc.getField(path);
    if (fieldElem.eoo()) {
        return false;
    }

    // Collect the document field's values into a sorted vector for merge-scan.
    std::vector<BSONElement> docValues;
    if (fieldElem.type() == BSONType::array) {
        BSONObjIterator iter(fieldElem.embeddedObject());
        while (iter.more()) {
            docValues.push_back(iter.next());
        }
    } else {
        docValues.push_back(fieldElem);
    }
    std::sort(docValues.begin(), docValues.end(), cmp);

    return std::includes(docValues.begin(),
                         docValues.end(),
                         sortedRequiredValues.begin(),
                         sortedRequiredValues.end(),
                         cmp);
}

}  // namespace

CardinalityEstimate SamplingEstimatorImpl::estimateCardinality(const MatchExpression* expr) const {
    tassert(10981500,
            "Sample must be generated before calling estimateCardinality()",
            _isSampleGenerated);
    if (_sampleSize == 0) {
        return cost_based_ranker::zeroCE;
    }

    if (!_topLevelSampleFieldNames.empty()) {
        checkSampleContainsMatchExpressionFields(_topLevelSampleFieldNames, expr);
    }

    size_t cnt = 0;
    size_t errorCount = 0;

    // Fast path for AND of equalities on the same field path (the common shape produced by $all).
    // The generic matcher evaluates each equality by scanning the array independently: O(N *
    // array_length) per sample document. This fast path sorts the required values once and uses a
    // merge-scan per document.
    if (auto allEqInfo = tryExtractAllEqualities(expr)) {
        auto& [path, requiredValues] = *allEqInfo;
        BSONElementCmpWithoutField cmp;
        // Sort required values
        std::sort(requiredValues.begin(), requiredValues.end(), cmp);
        for (const auto& doc : _sample) {
            if (documentMatchesAllEqualities(doc, path, requiredValues)) {
                ++cnt;
            }
        }
    } else {
        for (const auto& doc : _sample) {
            auto result = tryMatchesBSON(expr, doc);
            if (!result) {
                errorCount++;
                continue;
            }
            if (*result) {
                cnt++;
            }
        }
    }

    CardinalityEstimate estimate = makeScaledEstimate(cnt, errorCount, _sampleSize, getCollCard());

    LOGV2_DEBUG(9756604,
                5,
                "SamplingCE cardinality (# docs) for MatchExpression",
                "matchExpression"_attr = expr->toString(),
                "estimate"_attr = estimate);
    return estimate;
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateCardinality(
    const std::vector<const MatchExpression*>& expressions) const {
    tassert(10981501,
            "Sample must be generated before calling estimateCardinality()",
            _isSampleGenerated);
    if (!_topLevelSampleFieldNames.empty()) {
        checkSampleContainsMatchExpressionFields(_topLevelSampleFieldNames, expressions);
    }
    std::vector<double> counts(expressions.size(), 0);
    std::vector<size_t> errorCounts(expressions.size(), 0);
    // Experiment showed that this batch process performs better than calling
    // 'estimateCardinality(const MatchExpression* expr)' over and over.
    for (const auto& doc : _sample) {
        for (size_t i = 0; i < expressions.size(); i++) {
            auto result = tryMatchesBSON(expressions[i], doc);
            if (!result) {
                errorCounts[i]++;
                continue;
            }
            if (*result) {
                counts[i] += 1;
            }
        }
    }

    std::vector<CardinalityEstimate> estimates;
    for (size_t i = 0; i < counts.size(); i++) {
        estimates.push_back(
            makeScaledEstimate(counts[i], errorCounts[i], _sampleSize, getCollCard()));
    }

    return estimates;
}

CardinalityEstimate SamplingEstimatorImpl::estimateKeysScanned(const IndexBounds& bounds) const {
    tassert(10981502,
            "Sample must be generated before calling estimateKeysScanned()",
            _isSampleGenerated);
    if (bounds.isSimpleRange) {
        MONGO_UNIMPLEMENTED_TASSERT(9811500);
    }
    if (!_topLevelSampleFieldNames.empty()) {
        checkSampleContainsIndexBoundsFields(_topLevelSampleFieldNames, bounds);
    }

    size_t count = 0;

    forNumberKeysMatch(bounds, _sample, [&](size_t matchCnt) { count += matchCnt; });

    CardinalityEstimate estimate{CardinalityType{(count * getCollCard()) / _sampleSize},
                                 EstimationSource::Sampling};
    LOGV2_DEBUG(9756605,
                5,
                "SamplingCE cardinality (# keys) for index bounds",
                "interval"_attr = bounds.toString(false),
                "estimate"_attr = estimate);
    return estimate;
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateKeysScanned(
    const std::vector<const IndexBounds*>& bounds) const {
    tassert(10981503,
            "Sample must be generated before calling estimateKeysScanned()",
            _isSampleGenerated);
    std::vector<CardinalityEstimate> estimates;
    estimates.reserve(bounds.size());
    for (size_t i = 0; i < bounds.size(); i++) {
        estimates.push_back(estimateKeysScanned(*bounds[i]));
    }
    return estimates;
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateRIDs(
    const std::vector<const IndexBounds*>& bounds,
    const std::vector<const MatchExpression*>& expressions) const {
    tassert(10981504, "Sample must be generated before calling estimateRIDs()", _isSampleGenerated);

    std::vector<CardinalityEstimate> estimates;
    tassert(9942301,
            "bounds and expressions should have equal size.",
            expressions.size() == bounds.size());
    estimates.reserve(bounds.size());
    for (size_t i = 0; i < bounds.size(); i++) {
        estimates.push_back(estimateRIDs(*bounds[i], expressions[i]));
    }
    return estimates;
}

CardinalityEstimate SamplingEstimatorImpl::estimateRIDs(const IndexBounds& bounds,
                                                        const MatchExpression* expr) const {
    tassert(10981505, "Sample must be generated before calling estimateRIDs()", _isSampleGenerated);
    if (!_topLevelSampleFieldNames.empty()) {
        checkSampleContainsMatchExpressionFields(_topLevelSampleFieldNames, expr);
        checkSampleContainsIndexBoundsFields(_topLevelSampleFieldNames, bounds);
    }
    // Precompute the fast-path info for AND-of-same-path-equalities outside the bounds loop.
    boost::optional<std::pair<StringData, std::vector<BSONElement>>> allEqInfo;
    std::vector<BSONElement> sortedRequiredValues;
    if (expr) {
        allEqInfo = tryExtractAllEqualities(expr);
        if (allEqInfo) {
            BSONElementCmpWithoutField cmp;
            sortedRequiredValues = allEqInfo->second;
            std::sort(sortedRequiredValues.begin(), sortedRequiredValues.end(), cmp);
        }
    }

    size_t count = 0;
    size_t errorCount = 0;
    forDocumentsMatchingBounds(bounds, _sample, [&](const BSONObj& document) {
        // If 'expr' is null, we are simply estimating the cardinality by IndexBounds.
        if (expr == nullptr) {
            count++;
            return true;
        } else if (allEqInfo) {
            if (documentMatchesAllEqualities(document, allEqInfo->first, sortedRequiredValues)) {
                count++;
            }
            return true;
        } else {
            auto result = tryMatchesBSON(expr, document);
            if (!result) {
                errorCount++;
                return true;
            }
            if (*result) {
                count++;
            }
            return true;
        }
    });
    CardinalityEstimate estimate =
        makeScaledEstimate(count, errorCount, _sampleSize, getCollCard());
    LOGV2_DEBUG(9756606,
                5,
                "SamplingCE cardinality (# docs) for index bounds and MatchExpression",
                "interval"_attr = bounds.toString(false),
                "matchExpression"_attr = expr ? expr->toString() : "<none>",
                "estimate"_attr = estimate);
    return estimate;
}

SamplingEstimatorImpl::SamplingEstimatorImpl(OperationContext* opCtx,
                                             const MultipleCollectionAccessor& collections,
                                             const NamespaceString& nss,
                                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                                             size_t sampleSize,
                                             SamplingCEMethodEnum samplingStyle,
                                             boost::optional<int> numChunks,
                                             CardinalityEstimate collectionCard)
    : _sampleSize(sampleSize),
      _opCtx(opCtx),
      _collections(collections),
      _nss(nss),
      _yieldPolicy(yieldPolicy),
      _samplingStyle(samplingStyle),
      _numChunks(numChunks),
      _collectionCard(collectionCard) {}

SamplingEstimatorImpl::SamplingEstimatorImpl(OperationContext* opCtx,
                                             const MultipleCollectionAccessor& collections,
                                             const NamespaceString& nss,
                                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                                             SamplingCEMethodEnum samplingStyle,
                                             CardinalityEstimate collectionCard,
                                             SamplingConfidenceIntervalEnum ci,
                                             double marginOfError,
                                             boost::optional<int> numChunks)
    : SamplingEstimatorImpl(opCtx,
                            collections,
                            nss,
                            yieldPolicy,
                            calculateSampleSize(ci, marginOfError),
                            samplingStyle,
                            numChunks,
                            collectionCard) {}

SamplingEstimatorImpl::~SamplingEstimatorImpl() {}

CardinalityEstimate SamplingEstimatorImpl::estimateNDV(
    const std::vector<FieldPathAndEqSemantics>& fields,
    boost::optional<std::span<const OrderedIntervalList>> bounds) const {
    tassert(11158504, "Sample must be generated before calling estimateNDV()", _isSampleGenerated);

    if (!_topLevelSampleFieldNames.empty()) {
        for (const auto& field : fields) {
            tassert(11158505,
                    "Sample must include the NDV fieldName as a top-level field.",
                    _topLevelSampleFieldNames.contains(field.path.front()));
        }
    }

    // Obtain the NDV for the sample. Compare against the number of unique documents. If they are
    // equal, skip NR iteration since it is likely to diverge. The best guess is that each element
    // in the collection is unique. We compare sampleNDV against uniqueDocCount instead of
    // sampleSize because we sample with replacement. This can produce duplicate documents, which
    // may result in us oscillating in and out of NR for defacto unique fields. The unique doc count
    // is lazily computed and cached.
    size_t sampleNDV = countNDV(fields, _sample, bounds);
    if (!_uniqueDocCount) {
        _uniqueDocCount = countUniqueDocuments(_sample);
    }
    if (sampleNDV == *_uniqueDocCount) {
        LOGV2_DEBUG(11228302,
                    5,
                    "SamplingCE NDV is equal to the unique document count, outputting collection "
                    "size",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = sampleNDV,
                    "uniqueDocCount"_attr = *_uniqueDocCount,
                    "sampleSize"_attr = _sampleSize,
                    "collectionCard"_attr = _collectionCard);
        return _collectionCard;
    }

    tassert(12237901,
            "Non-bounded NDV count should never be zero",
            bounds.has_value() || sampleNDV != 0);
    if (bounds.has_value() && sampleNDV == 0) {
        return cost_based_ranker::zeroCE;
    }

    // Note that we use '_sampleSize' instead of '_uniqueDocCount' here because the method of
    // moments estimator that we use assumes that we perform sampling with replacement.
    CardinalityEstimate estimate = newtonRaphsonNDV(sampleNDV, _sampleSize);
    LOGV2_DEBUG(11158506,
                5,
                "SamplingCE ndv (# unique values) for field",
                "fields"_attr = fields,
                "sampleNDV"_attr = sampleNDV,
                "estimate"_attr = estimate);

    if (estimate > _collectionCard) {
        LOGV2_DEBUG(11158507,
                    5,
                    "SamplingCE ndv exceeds collection size, rounding down",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = sampleNDV,
                    "estimate"_attr = estimate,
                    "collectionCard"_attr = _collectionCard);
        estimate = _collectionCard;
    }
    return estimate;
}

CardinalityEstimate SamplingEstimatorImpl::estimateNDVMultiKey(
    const std::vector<FieldPathAndEqSemantics>& fields,
    boost::optional<std::span<const OrderedIntervalList>> bounds) const {
    tassert(10061103, "Sample must be generated before calling estimateNDV()", _isSampleGenerated);

    if (!_topLevelSampleFieldNames.empty()) {
        for (const auto& field : fields) {
            tassert(10061104,
                    "Sample must include the NDV fieldName as a top-level field.",
                    _topLevelSampleFieldNames.contains(field.path.front()));
        }
    }

    // Obtain the NDV for the sample. If this is equal to the sample size, don't bother with NR
    // iteration, since it is likely to diverge. The best guess is that each element in the
    // collection is unique.
    const auto [totalSampleKeys, totalMatchingKeys, totalUniqueKeys] =
        countNDVMultiKey(fields, _sample, bounds);

    if (bounds.has_value() && !totalMatchingKeys) {
        return cost_based_ranker::zeroCE;
    }

    const auto avgKeysPerDoc = (double(totalSampleKeys) / _sample.size());
    const auto estimatedIndexKeys = _collectionCard * avgKeysPerDoc;
    if (totalUniqueKeys == totalMatchingKeys) {
        LOGV2_DEBUG(10061105,
                    5,
                    "SamplingCE NDV is equal to the sample size, outputting estimated index size",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = totalUniqueKeys,
                    "estimatedIndexKeys"_attr = estimatedIndexKeys);
        return estimatedIndexKeys;
    }

    CardinalityEstimate estimate = newtonRaphsonNDV(totalUniqueKeys, totalSampleKeys);
    LOGV2_DEBUG(10061106,
                5,
                "SamplingCE ndv (# unique values) for field",
                "fields"_attr = fields,
                "sampleNDV"_attr = totalUniqueKeys,
                "estimate"_attr = estimate);

    if (estimate > estimatedIndexKeys) {
        LOGV2_DEBUG(10061107,
                    5,
                    "SamplingCE ndv exceeds estimated index size, rounding down",
                    "fields"_attr = fields,
                    "sampleNDV"_attr = totalUniqueKeys,
                    "estimate"_attr = estimate,
                    "estimatedIndexKeys"_attr = estimatedIndexKeys);
        estimate = estimatedIndexKeys;
    }
    return estimate;
}

std::unique_ptr<SamplingEstimator> SamplingEstimatorImpl::makeDefaultSamplingEstimator(
    const CanonicalQuery& cq,
    CardinalityEstimate collCard,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections) {
    const auto& qkc = cq.getExpCtx()->getQueryKnobConfiguration();
    return std::unique_ptr<ce::SamplingEstimatorImpl>(
        new ce::SamplingEstimatorImpl(cq.getOpCtx(),
                                      collections,
                                      cq.nss(),
                                      yieldPolicy,
                                      qkc.getInternalQuerySamplingCEMethod(),
                                      collCard,
                                      qkc.getConfidenceInterval(),
                                      qkc.getSamplingMarginOfError(),
                                      qkc.getNumChunksForChunkBasedSampling()));
}

}  // namespace mongo::ce
