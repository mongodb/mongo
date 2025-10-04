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

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/util/assert_util.h"

#include <cmath>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {

using CardinalityType = mongo::cost_based_ranker::CardinalityType;
using EstimationSource = mongo::cost_based_ranker::EstimationSource;

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
                                              boost::optional<sbe::value::SlotId> seekRecordIdSlot,
                                              bool useRandomCursor,
                                              PlanYieldPolicy* sbeYieldPolicy) {
    sbe::value::SlotVector scanFieldSlots;
    std::vector<std::string> scanFieldNames;
    sbe::ScanCallbacks callbacks{};
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
                                      seekRecordIdSlot,
                                      boost::none /* minRecordIdSlot */,
                                      boost::none /* maxRecordIdSlot */,
                                      true /* forward */,
                                      sbeYieldPolicy,
                                      0 /* nodeId */,
                                      callbacks,
                                      useRandomCursor);
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
        std::make_unique<sbe::MakeObjSpec>(FieldListScope::kClosed,
                                           std::move(topLevelSampleFieldNamesVec),
                                           std::move(fieldActions),
                                           sbe::MakeObjSpec::NonObjInputBehavior::kReturnNothing);
    auto specExpr =
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::makeObjSpec,
                                   sbe::value::bitcastFrom<sbe::MakeObjSpec*>(spec.release()));

    // Create a built-in SBE makeBsonObj func and pass in the 2 args: MakeObjSpec and slot to read
    // input doc from (inputSlot).
    auto func = sbe::makeE<sbe::EFunction>(
        "makeBsonObj"_sd, sbe::makeEs(std::move(specExpr), sbe::makeE<sbe::EVariable>(inputSlot)));
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

std::vector<BSONObj> SamplingEstimatorImpl::getIndexKeys(const IndexBounds& bounds,
                                                         const BSONObj& doc) {
    std::vector<const char*> fieldNames;
    fieldNames.reserve(bounds.fields.size());
    for (size_t i = 0; i < bounds.fields.size(); ++i) {
        fieldNames.push_back(bounds.fields[i].name.c_str());
    }
    std::vector<BSONElement> keysElements{fieldNames.size(), BSONElement{}};

    auto bTreeKeyGenerator =
        std::make_unique<BtreeKeyGenerator>(fieldNames,
                                            keysElements,
                                            false /*isSparse*/,
                                            key_string::Version::kLatestVersion,
                                            Ordering::allAscending());

    MultikeyPaths multikeyPaths;
    SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);
    KeyStringSet keyStrings;

    bTreeKeyGenerator->getKeys(pooledBufferBuilder,
                               doc,
                               false /*skipMultikey*/,
                               &keyStrings,
                               &multikeyPaths,
                               nullptr /*collator*/,
                               boost::none);

    std::vector<BSONObj> indexKeys;
    for (auto&& keyString : keyStrings) {
        indexKeys.push_back(key_string::toBson(keyString, Ordering::make(BSONObj())));
    }
    return indexKeys;
}

bool SamplingEstimatorImpl::matches(const Interval& interval, BSONElement val) {
    int startCmp = val.woCompare(interval.start, 0 /*ignoreFieldNames*/);
    int endCmp = val.woCompare(interval.end, 0 /*ignoreFieldNames*/);

    if (startCmp == 0) {
        /**
         * The document value is equal to the starting point of the interval; the document is inside
         * the bounds of this index interval if the starting point is included in the interval.
         */
        return interval.startInclusive;
    } else if (startCmp < 0 && endCmp < 0) {
        /**
         * The document value is less than both the starting point and the end point and is thus
         * not inside the bounds of this index interval. Depending on the index spec and the
         * direction the index is traversed, endCmp can be < startCmp which is why it's necesary to
         * check both interval end points.
         */
        return false;
    }

    if (endCmp == 0) {
        /**
         * The document value is equal to the end point of the interval; the document is inside the
         * bounds of this index interval if the end point is included in the interval.
         */
        return interval.endInclusive;
    } else if (endCmp > 0 && startCmp > 0) {
        /**
         * The document value is greater than both the starting point and the end point and is thus
         * not inside the bounds of this index interval. Depending on the index spec and the
         * direction the index is traversed, startCmp can be < endCmp which is why it's necesary to
         * check both interval end points.
         */
        return false;
    }
    return true;
}

bool SamplingEstimatorImpl::matches(const OrderedIntervalList& oil, BSONElement val) {
    for (auto&& interval : oil.intervals) {
        if (matches(interval, val)) {
            return true;
        }
    }
    return false;
}

bool SamplingEstimatorImpl::doesKeyMatchBounds(const IndexBounds& bounds,
                                               const BSONObj& key) const {
    BSONObjIterator it(key);
    for (auto&& oil : bounds.fields) {
        if (!matches(oil, *it)) {
            return false;
        }
        ++it;
    }
    return true;
}

size_t SamplingEstimatorImpl::numberKeysMatch(const IndexBounds& bounds,
                                              const BSONObj& doc,
                                              bool skipDuplicateMatches) const {
    const auto keys = getIndexKeys(bounds, doc);
    size_t count = 0;
    for (auto&& key : keys) {
        if (doesKeyMatchBounds(bounds, key)) {
            count++;
            if (skipDuplicateMatches) {
                return count;
            }
        }
    }
    return count;
}

bool SamplingEstimatorImpl::doesDocumentMatchBounds(const IndexBounds& bounds,
                                                    const BSONObj& doc) const {
    return numberKeysMatch(bounds, doc, true /* skipDuplicateMatches */) > 0;
}

size_t SamplingEstimatorImpl::calculateSampleSize(SamplingConfidenceIntervalEnum ci,
                                                  double marginOfError) {
    uassert(9406301, "Margin of error should be larger than 0.", marginOfError > 0);
    double z = getZScore(ci);
    double ciWidth = 2 * marginOfError / 100.0;

    return static_cast<size_t>(std::lround((z * z) / (ciWidth * ciWidth)));
}

std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
SamplingEstimatorImpl::generateRandomSamplingPlan(PlanYieldPolicy* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();
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
SamplingEstimatorImpl::generateChunkSamplingPlan(PlanYieldPolicy* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();
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
    LOGV2_DEBUG(10670302,
                5,
                "SamplingCE Sampling SBE plan",
                "SBE Plan"_attr = sbe::DebugPrinter{}.print(plan.first.get()->debugPrint()));
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
                                                             _collections,
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

    _sampleSize = _sample.size();
}

void SamplingEstimatorImpl::generateFullCollScanSample() {
    // Create a CanonicalQuery for the CollScan plan.
    const auto& ns = _collections.getMainCollection()->ns();
    auto cq = makeEmptyCanonicalQuery(ns, _opCtx);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, ns);

    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();

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
    const auto& ns = _collections.getMainCollection()->ns();
    auto cq = makeEmptyCanonicalQuery(ns, _opCtx);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, ns);

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
    const auto& ns = _collections.getMainCollection()->ns();
    auto cq = makeEmptyCanonicalQuery(ns, _opCtx);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, ns);

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
    if (internalQuerySamplingBySequentialScan.load()) {
        // This is only used for testing purposes when a repeatable sample is needed.
        generateSampleBySeqScanningForTesting();
        return;
    }

    if (_sampleSize >= _collectionCard.cardinality().v()) {
        // If the required sample is larger than the collection, the sample is generated from all
        // the documents on the collection.
        generateFullCollScanSample();
    } else if (_samplingStyle == SamplingStyle::kRandom) {
        generateRandomSample();
    } else {
        tassert(9372901, "The number of chunks should be positive.", _numChunks && *_numChunks > 0);
        generateChunkSample();
    }
}

void SamplingEstimatorImpl::generateSampleBySeqScanningForTesting() {
    // Create a CanonicalQuery for the sampling plan.
    const auto& ns = _collections.getMainCollection()->ns();
    auto cq = makeEmptyCanonicalQuery(ns, _opCtx);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, ns);

    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();
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
    try {
        for (const auto& doc : _sample) {
            if (exec::matcher::matchesBSON(expr, doc, nullptr)) {
                cnt++;
            }
        }
    } catch (const DBException&) {
        // The evaluation of this expression failed. This estimation stops here and returns a
        // CardinalityEstimate calculated based on whatever we have counted at this point with a
        // erroneous status.
        CardinalityEstimate ce{CardinalityType{(cnt * getCollCard()) / _sampleSize},
                               EstimationSource::Sampling};
        return ce;
    }
    CardinalityEstimate estimate{CardinalityType{(cnt * getCollCard()) / _sampleSize},
                                 EstimationSource::Sampling};
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
    // The bool value indicates whether the estimation of the corresponding expression was aborted
    // or not. The estimation would be stopped if any error occurred during the estimation.
    std::vector<std::pair<double, bool>> estimates(expressions.size(), {0, true});
    // Experiment showed that this batch process performs better than calling
    // 'estimateCardinality(const MatchExpression* expr)' over and over.
    for (const auto& doc : _sample) {
        for (size_t i = 0; i < expressions.size(); i++) {
            if (estimates[i].second) {
                try {
                    if (exec::matcher::matchesBSON(expressions[i], doc, nullptr)) {
                        estimates[i].first += 1;
                    }
                } catch (const DBException&) {
                    // The expression failed to evaluate. Abort the estimation of this expression.
                    estimates[i].second = false;
                }
            }
        }
    }

    std::vector<CardinalityEstimate> estimatesCard;
    for (auto card : estimates) {
        double estimate = (card.first * getCollCard()) / _sampleSize;
        estimatesCard.push_back(
            CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling});
    }

    return estimatesCard;
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
    for (auto&& doc : _sample) {
        count += numberKeysMatch(bounds, doc);
    }
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
    size_t count = 0;
    for (auto&& doc : _sample) {
        if (doesDocumentMatchBounds(bounds, doc)) {
            // If 'expr' is null, we are simply estimating the cardinality by IndexBounds.
            if (expr == nullptr || exec::matcher::matchesBSON(expr, doc, nullptr)) {
                count++;
            }
        }
    }
    CardinalityEstimate estimate{CardinalityType{(count * getCollCard()) / _sampleSize},
                                 EstimationSource::Sampling};
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
                                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                                             size_t sampleSize,
                                             SamplingStyle samplingStyle,
                                             boost::optional<int> numChunks,
                                             CardinalityEstimate collectionCard)
    : _opCtx(opCtx),
      _collections(collections),
      _yieldPolicy(yieldPolicy),
      _samplingStyle(samplingStyle),
      _sampleSize(sampleSize),
      _numChunks(numChunks),
      _collectionCard(collectionCard) {}

SamplingEstimatorImpl::SamplingEstimatorImpl(OperationContext* opCtx,
                                             const MultipleCollectionAccessor& collections,
                                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                                             SamplingStyle samplingStyle,
                                             CardinalityEstimate collectionCard,
                                             SamplingConfidenceIntervalEnum ci,
                                             double marginOfError,
                                             boost::optional<int> numChunks)
    : SamplingEstimatorImpl(opCtx,
                            collections,
                            yieldPolicy,
                            calculateSampleSize(ci, marginOfError),
                            samplingStyle,
                            numChunks,
                            collectionCard) {}

SamplingEstimatorImpl::~SamplingEstimatorImpl() {}

}  // namespace mongo::ce
