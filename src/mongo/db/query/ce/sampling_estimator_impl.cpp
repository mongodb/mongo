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

namespace mongo::ce {

using CardinalityType = mongo::cost_based_ranker::CardinalityType;
using EstimationSource = mongo::cost_based_ranker::EstimationSource;

std::unique_ptr<CanonicalQuery> SamplingEstimatorImpl::makeCanonicalQuery(
    const NamespaceString& nss, OperationContext* opCtx, size_t sampleSize) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(nss));
    findCommand->setLimit(sampleSize);

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

    // This function converts an index key to a BSONObj in order to compare with the IndexBounds.
    auto keyStringToBson = [](const key_string::Value& keyString) {
        BSONObjBuilder bob;
        auto keyStringObj = key_string::toBson(keyString, Ordering::make(BSONObj()));
        for (auto&& keyStringElem : keyStringObj) {
            bob.append(keyStringElem);
        }
        return bob.obj();
    };

    std::vector<BSONObj> indexKeys;
    for (auto&& keyString : keyStrings) {
        indexKeys.push_back(keyStringToBson(keyString));
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

std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
SamplingEstimatorImpl::generateChunkSamplingPlan(PlanYieldPolicy* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    const CollectionPtr& collection = _collections.getMainCollection();
    auto chunkSize = static_cast<int64_t>(_sampleSize / *_numChunks);

    boost::optional<sbe::value::SlotId> outerRid = ids.generate();
    auto outerStage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                                 collection->ns().dbName(),
                                                 boost::none /* recordSlot */,
                                                 outerRid /* recordIdSlot */,
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
                                                 true /* useRandomCursor */);

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
    auto innerStage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
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
                                                 outerRid /* seekRecordIdSlot */,
                                                 boost::none /* minRecordIdSlot */,
                                                 boost::none /* maxRecordIdSlot */,
                                                 true /* forward */,
                                                 sbeYieldPolicy,
                                                 0 /* nodeId */,
                                                 sbe::ScanCallbacks{},
                                                 false /* useRandomCursor */);

    innerStage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(innerStage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(chunkSize)),
        nullptr /* skip */,
        0 /* nodeId */);


    auto loopJoinStage = sbe::makeS<sbe::LoopJoinStage>(std::move(outerStage),
                                                        std::move(innerStage),
                                                        sbe::value::SlotVector{},
                                                        sbe::value::SlotVector{*outerRid},
                                                        nullptr /* predicate */,
                                                        0 /* _nodeId */);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(loopJoinStage), std::move(data)};
}

void SamplingEstimatorImpl::generateRandomSample(size_t sampleSize) {
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
    // The real sample size could be a bit less than the size required in the case that there's any
    // chunk hitting the end of the collection.
    _sampleSize = _sample.size();

    return;
}

void SamplingEstimatorImpl::generateChunkSample() {
    generateChunkSample(_sampleSize);
    return;
}

CardinalityEstimate SamplingEstimatorImpl::estimateCardinality(const MatchExpression* expr) const {
    size_t cnt = 0;
    for (const auto& doc : _sample) {
        if (expr->matchesBSON(doc, nullptr)) {
            cnt++;
        }
    }
    double estimate = (cnt * getCollCard()) / _sampleSize;
    return CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling};
}

std::vector<CardinalityEstimate> SamplingEstimatorImpl::estimateCardinality(
    const std::vector<MatchExpression*>& expressions) const {
    std::vector<CardinalityEstimate> estimates;
    for (auto& expr : expressions) {
        estimates.push_back(estimateCardinality(expr));
    }

    return estimates;
}

CardinalityEstimate SamplingEstimatorImpl::estimateKeysScanned(const IndexBounds& bounds) const {
    if (bounds.isSimpleRange) {
        MONGO_UNIMPLEMENTED_TASSERT(9811500);
    }
    size_t count = 0;
    for (auto&& doc : _sample) {
        count += numberKeysMatch(bounds, doc);
    }
    double estimate = (count * getCollCard()) / _sampleSize;
    return CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling};
}

CardinalityEstimate SamplingEstimatorImpl::estimateRIDs(const IndexBounds& bounds,
                                                        const MatchExpression* expr) const {
    size_t count = 0;
    for (auto&& doc : _sample) {
        if (doesDocumentMatchBounds(bounds, doc)) {
            // If 'expr' is null, we are simply estimating the cardinality by IndexBounds.
            if (expr == nullptr || expr->matchesBSON(doc, nullptr)) {
                count++;
            }
        }
    }
    double estimate = (count * getCollCard()) / _sampleSize;
    return CardinalityEstimate{CardinalityType{estimate}, EstimationSource::Sampling};
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

    uassert(9406300,
            "Sample size cannot be larger than collection cardinality. The sample size can be "
            "reduced by choosing a larger margin of error.",
            (double)sampleSize <= collectionCard.cardinality().v());

    if (samplingStyle == SamplingStyle::kRandom) {
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
