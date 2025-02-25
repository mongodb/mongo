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

#include "mongo/db/query/ce/sampling_estimator_impl.h"

#include <cmath>

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/cost_based_ranker/estimates.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/platform/basic.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {

using CardinalityType = mongo::cost_based_ranker::CardinalityType;
using EstimationSource = mongo::cost_based_ranker::EstimationSource;

std::unique_ptr<CanonicalQuery> SamplingEstimatorImpl::makeCanonicalQuery(
    const NamespaceString& nss, OperationContext* opCtx, boost::optional<size_t> sampleSize) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(nss));
    if (sampleSize) {
        findCommand->setLimit(*sampleSize);
    }

    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build();

    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx,
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    return std::move(statusWithCQ.getValue());
}

namespace {
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
                                      boost::none /* oplogTsSlot */,
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
}  // namespace

std::vector<BSONObj> SamplingEstimatorImpl::getIndexKeys(const IndexBounds& bounds,
                                                         const BSONObj& doc) {
    std::vector<const char*> fieldNames;
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
    if (startCmp == 0) {
        return interval.startInclusive;
    } else if (startCmp < 0) {
        return false;
    }
    int endCmp = val.woCompare(interval.end, 0 /*ignoreFieldNames*/);
    if (endCmp == 0) {
        return interval.endInclusive;
    } else if (endCmp > 0) {
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
    auto loopJoinStage = sbe::makeS<sbe::LoopJoinStage>(std::move(outerStage),
                                                        std::move(innerStage),
                                                        outerProjectsSlots,
                                                        outerCorrelatedSlots,
                                                        nullptr /* predicate */,
                                                        0 /* _nodeId */);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(loopJoinStage), std::move(data)};
}

void SamplingEstimatorImpl::executeSamplingQueryAndSample(
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>& plan,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy) {
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

    _sampleSize = _sample.size();
}

void SamplingEstimatorImpl::generateFullCollScanSample() {
    // Create a CanonicalQuery for the CollScan plan.
    auto cq = makeCanonicalQuery(_collections.getMainCollection()->ns(), _opCtx, boost::none);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(
        _opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, _collections, cq->nss());

    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();

    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, false, sbeYieldPolicy.get());
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
    auto cq = makeCanonicalQuery(_collections.getMainCollection()->ns(), _opCtx, sampleSize);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(
        _opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, _collections, cq->nss());

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
    auto cq = makeCanonicalQuery(_collections.getMainCollection()->ns(), _opCtx, sampleSize);
    _sampleSize = sampleSize;
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(
        _opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, _collections, cq->nss());

    auto plan = generateChunkSamplingPlan(sbeYieldPolicy.get());
    executeSamplingQueryAndSample(plan, std::move(cq), std::move(sbeYieldPolicy));

    return;
}

void SamplingEstimatorImpl::generateChunkSample() {
    generateChunkSample(_sampleSize);
    return;
}

void SamplingEstimatorImpl::generateSampleBySeqScanningForTesting() {
    // Create a CanonicalQuery for the sampling plan.
    auto cq = makeCanonicalQuery(_collections.getMainCollection()->ns(), _opCtx, _sampleSize);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(
        _opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, _collections, cq->nss());

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
    size_t cnt = 0;
    for (const auto& doc : _sample) {
        if (exec::matcher::matchesBSON(expr, doc, nullptr)) {
            cnt++;
        }
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
    // Experiment showed that this batch process performs better than calling
    // 'estimateCardinality(const MatchExpression* expr)' over and over.
    std::vector<double> estimates(expressions.size(), 0);
    for (const auto& doc : _sample) {
        for (size_t i = 0; i < expressions.size(); i++) {
            if (exec::matcher::matchesBSON(expressions[i], doc, nullptr)) {
                estimates[i] += 1;
            }
        }
    }

    std::vector<CardinalityEstimate> estimatesCard;
    for (auto card : estimates) {
        double estimate = (card * getCollCard()) / _sampleSize;
        estimatesCard.push_back(
            CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling});
    }

    return estimatesCard;
}

CardinalityEstimate SamplingEstimatorImpl::estimateKeysScanned(const IndexBounds& bounds) const {
    if (bounds.isSimpleRange) {
        MONGO_UNIMPLEMENTED_TASSERT(9811500);
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
    std::vector<CardinalityEstimate> estimates;
    for (size_t i = 0; i < bounds.size(); i++) {
        estimates.push_back(estimateKeysScanned(*bounds[i]));
    }
    return estimates;
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateRIDs(
    const std::vector<const IndexBounds*>& bounds,
    const std::vector<const MatchExpression*>& expressions) const {
    std::vector<CardinalityEstimate> estimates;
    tassert(9942301,
            "bounds and expressions should have equal size.",
            expressions.size() == bounds.size());
    for (size_t i = 0; i < bounds.size(); i++) {
        estimates.push_back(estimateRIDs(*bounds[i], expressions[i]));
    }
    return estimates;
}

CardinalityEstimate SamplingEstimatorImpl::estimateRIDs(const IndexBounds& bounds,
                                                        const MatchExpression* expr) const {
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
                                             size_t sampleSize,
                                             SamplingStyle samplingStyle,
                                             boost::optional<int> numChunks,
                                             CardinalityEstimate collectionCard)
    : _opCtx(opCtx),
      _collections(collections),
      _sampleSize(sampleSize),
      _numChunks(numChunks),
      _collectionCard(collectionCard) {
    if (internalQuerySamplingBySequentialScan.load()) {
        // This is only used for testing purposes when a repeatable sample is needed.
        generateSampleBySeqScanningForTesting();
        return;
    }

    if (sampleSize >= collectionCard.cardinality().v()) {
        // If the required sample is larger than the collection, the sample is generated from all
        // the documents on the collection.
        generateFullCollScanSample();
    } else if (samplingStyle == SamplingStyle::kRandom) {
        generateRandomSample();
    } else {
        tassert(9372901, "The number of chunks should be positive.", numChunks && *numChunks > 0);
        generateChunkSample();
    }
}

SamplingEstimatorImpl::SamplingEstimatorImpl(OperationContext* opCtx,
                                             const MultipleCollectionAccessor& collections,
                                             SamplingStyle samplingStyle,
                                             CardinalityEstimate collectionCard,
                                             SamplingConfidenceIntervalEnum ci,
                                             double marginOfError,
                                             boost::optional<int> numChunks)
    : SamplingEstimatorImpl(opCtx,
                            collections,
                            calculateSampleSize(ci, marginOfError),
                            samplingStyle,
                            numChunks,
                            collectionCard) {}

SamplingEstimatorImpl::~SamplingEstimatorImpl() {}

}  // namespace mongo::ce
