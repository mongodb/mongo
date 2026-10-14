// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/sampling/on_the_fly_sample_loader.h"

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/random_scan.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/ce/sampling/sample_loader.h"
#include "mongo/db/query/compiler/ce/sampling/sample_spec.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::ce {
using namespace std::literals::string_view_literals;

namespace {

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
                                                /*snapshotIdSlot=*/boost::none,
                                                /*indexIdentSlot=*/boost::none,
                                                /*indexKeySlot=*/boost::none,
                                                /*keyPatternSlot=*/boost::none,
                                                scanFieldNames,
                                                scanFieldSlots,
                                                sbeYieldPolicy,
                                                /*nodeId=*/0);
    } else if (minRecordIdSlot) {
        return sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                          collection->ns().dbName(),
                                          recordSlot,
                                          recordIdSlot,
                                          /*snapshotIdSlot=*/boost::none,
                                          /*indexIdentSlot=*/boost::none,
                                          /*indexKeySlot=*/boost::none,
                                          /*keyPatternSlot=*/boost::none,
                                          scanFieldNames,
                                          scanFieldSlots,
                                          minRecordIdSlot,
                                          /*maxRecordIdSlot=*/boost::none,
                                          /*forward=*/true,
                                          sbeYieldPolicy,
                                          /*nodeId=*/0,
                                          std::move(scanOpenCallback));
    }
    return sbe::makeS<sbe::GenericScanStage>(collection->uuid(),
                                             collection->ns().dbName(),
                                             recordSlot,
                                             recordIdSlot,
                                             /*snapshotIdSlot=*/boost::none,
                                             /*indexIdentSlot=*/boost::none,
                                             /*indexKeySlot=*/boost::none,
                                             /*keyPatternSlot=*/boost::none,
                                             scanFieldNames,
                                             scanFieldSlots,
                                             /*forward=*/true,
                                             sbeYieldPolicy,
                                             /*nodeId=*/0,
                                             std::move(scanOpenCallback));
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
    fieldActions.reserve(topLevelSampleFieldNames.size());
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
    return sbe::makeProjectStage(std::move(stage), /*nodeId=*/0, outputSlot, std::move(func));
}

/**
 * Applies the spec's inclusion projection to 'stage', if the spec requests one.
 */
void maybeInjectProjection(const SampleSpec& spec,
                           sbe::value::SlotIdGenerator& ids,
                           std::unique_ptr<sbe::PlanStage>& stage,
                           std::unique_ptr<stage_builder::PlanStageStaticData>& staticData) {
    if (spec.topLevelSampleFieldNames.empty()) {
        return;
    }
    auto projectResultSlot = ids.generate();
    stage = makeProjectStage(std::move(stage),
                             staticData->resultSlot.get(),
                             projectResultSlot,
                             spec.topLevelSampleFieldNames);
    staticData->resultSlot = projectResultSlot;
}

/**
 * This helper creates a CanonicalQuery for the sampling plan. This CanonicalQuery is "empty"
 * because its sole purpose is to be passed to 'prepareSlotBasedExecutableTree()' as part of
 * preparing the sampling plan for execution in SBE. That function uses the CanonicalQuery to
 * bind input parameters, but this is a no-op for sampling CE.
 */
std::unique_ptr<CanonicalQuery> makeEmptyCanonicalQuery(
    const NamespaceString& nss,
    OperationContext* opCtx,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(nss));
    ExpressionContextBuilder builder;
    builder.fromRequest(opCtx, *findCommand);
    if (customerQueryExpCtx) {
        builder.pathArraynessFrom(*customerQueryExpCtx)
            .nonArrayPathsForNssFrom(*customerQueryExpCtx);
    }
    auto samplingQueryExpCtx = builder.build();
    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = samplingQueryExpCtx,
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    return std::move(statusWithCQ.getValue());
}

/**
 * Builds the sampling plan and returns the sample.
 */
Sample executeSamplingQueryAndSample(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const NamespaceString& nss,
    std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>& plan,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy,
    SamplingTechniqueEnum technique) {
    if (logv2::shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(5))) {
        sbe::DebugPrintInfo debugPrintInfo{};
        LOGV2_DEBUG(10670302,
                    5,
                    "SamplingCE Sampling SBE plan",
                    "SBE Plan"_attr =
                        sbe::DebugPrinter{}.print(plan.first.get()->debugPrint(debugPrintInfo)));
    }
    // Prepare the SBE plan for execution.
    prepareSlotBasedExecutableTree(opCtx,
                                   plan.first.get(),
                                   &plan.second,
                                   *cq,
                                   collections,
                                   sbeYieldPolicy.get(),
                                   /*preparingFromSbeCache=*/false);

    // Create a PlanExecutor for the execution of the sampling plan.
    auto exec = mongo::plan_executor_factory::make(opCtx,
                                                   std::move(cq),
                                                   /*solution=*/nullptr,
                                                   std::move(plan),
                                                   collections,
                                                   QueryPlannerParams::DEFAULT,
                                                   nss,
                                                   std::move(sbeYieldPolicy),
                                                   /*isFromPlanCache=*/false,
                                                   /*cachedPlanHash=*/false);

    std::vector<BSONObj> docs;
    BSONObj obj;
    // Execute the plan, exhaust results and cache the sample.
    while (PlanExecutor::ADVANCED == exec->getNext(&obj, nullptr)) {
        docs.push_back(obj.getOwned());
    }
    return Sample::makeOnTheFly(std::move(docs), technique);
}

/**
 * Constructs a sampling SBE plan using the random-walk method. The SBE plan consists of a
 * sbe::ScanStage which uses a random cursor to read documents randomly from the collection and a
 * sbe::LimitSkipStage on the top of the scan stage to limit the requested number of documents.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
makeRandomSamplingPlan(const SampleSpec& spec,
                       const CollectionPtr& collection,
                       PlanYieldPolicySBE* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, true, sbeYieldPolicy);

    stage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(stage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(spec.sampleSize)),
        /*skip=*/nullptr,
        /*nodeId=*/0);

    maybeInjectProjection(spec, ids, stage, staticData);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(stage), std::move(data)};
}

/**
 * Constructs a sampling SBE plan using the chunk-based method.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
makeChunkSamplingPlan(const SampleSpec& spec,
                      const CollectionPtr& collection,
                      PlanYieldPolicySBE* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    auto chunkSize = static_cast<int64_t>(spec.sampleSize / *spec.numChunks);

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
                                   sbe::value::bitcastFrom<int64_t>(*spec.numChunks)),
        /*skip=*/nullptr,
        /*nodeId=*/0);

    // The inner stage seeks into the starting point picked by the outer stage and scans 'chunkSize'
    // documents sequentially.
    auto innerStage = makeScanStage(
        collection, staticData->resultSlot, boost::none, outerRid, false, sbeYieldPolicy);

    innerStage = sbe::makeS<sbe::LimitSkipStage>(
        std::move(innerStage),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                   sbe::value::bitcastFrom<int64_t>(chunkSize)),
        /*skip=*/nullptr,
        /*nodeId=*/0);


    sbe::value::SlotVector outerProjectsSlots;
    sbe::value::SlotVector outerCorrelatedSlots{*outerRid};
    auto stage = sbe::makeS<sbe::LoopJoinStage>(std::move(outerStage),
                                                std::move(innerStage),
                                                outerProjectsSlots,
                                                outerCorrelatedSlots,
                                                /*predicate=*/nullptr,
                                                /*_nodeId=*/0);

    maybeInjectProjection(spec, ids, stage, staticData);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};

    return {std::move(stage), std::move(data)};
}

/**
 * Constructs a full collection scan plan. The sample is generated when the collection size is
 * smaller than the required sample size.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
makeFullCollScanPlan(const SampleSpec& spec,
                     const CollectionPtr& collection,
                     PlanYieldPolicySBE* sbeYieldPolicy) {
    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, false, sbeYieldPolicy);

    maybeInjectProjection(spec, ids, stage, staticData);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};
    return {std::move(stage), std::move(data)};
}

/**
 * Builds the plan for the repeatable testing techniques: a sequential scan capped at the requested
 * sample size, optionally with the strides hash-mod filter.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, mongo::stage_builder::PlanStageData>
makeTestSamplingPlan(const SampleSpec& spec,
                     const CollectionPtr& collection,
                     OperationContext* opCtx,
                     SamplingTechniqueEnum technique,
                     PlanYieldPolicySBE* sbeYieldPolicy) {
    tassert(12433205,
            "Only kSeqScan and kStrides have a testing plan",
            technique == SamplingTechniqueEnum::kSeqScan ||
                technique == SamplingTechniqueEnum::kStrides);

    auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
    sbe::value::SlotIdGenerator ids;
    staticData->resultSlot = ids.generate();
    auto stage = makeScanStage(
        collection, staticData->resultSlot, boost::none, boost::none, false, sbeYieldPolicy);

    switch (technique) {
        case SamplingTechniqueEnum::kSeqScan: {
            // Scan the first 'sampleSize' documents sequentially from the start of the target
            // collection in order to generate a repeatable sample.
            stage = sbe::makeS<sbe::LimitSkipStage>(
                std::move(stage),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           sbe::value::bitcastFrom<int64_t>(spec.sampleSize)),
                /*skip=*/nullptr,
                /*nodeId=*/0);
            break;
        }
        case SamplingTechniqueEnum::kStrides: {
            const auto collCard = static_cast<size_t>(collection->numRecords(opCtx));
            tassert(12433206,
                    "Sample size must be greater than zero for stride sampling",
                    spec.sampleSize > 0);
            const auto strideModulus =
                std::max(int64_t{1}, static_cast<int64_t>(collCard / spec.sampleSize));

            // Sequentially scan the collection and keep documents whose _id satisfies
            // shardHash(_id) % strideModulus == 0, then cap the sample at 'sampleSize' documents.
            // Use shardHash (BSONElementHasher) rather than hash (Abseil) because Abseil hash
            // values are randomized per process and are not stable across process restarts.
            auto hashed = sbe::makeE<sbe::EFunction>(
                sbe::EFn::kShardHash,
                sbe::makeEs(sbe::makeE<sbe::EFunction>(
                    sbe::EFn::kGetField,
                    sbe::makeEs(sbe::makeE<sbe::EVariable>(staticData->resultSlot.get()),
                                sbe::makeE<sbe::EConstant>("_id"sv)))));
            auto modded = sbe::makeE<sbe::EFunction>(
                sbe::EFn::kMod,
                sbe::makeEs(
                    std::move(hashed),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                               sbe::value::bitcastFrom<int64_t>(strideModulus))));
            auto hashModFilter = sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::eq,
                std::move(modded),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           sbe::value::bitcastFrom<int64_t>(0)));
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), std::move(hashModFilter), /*nodeId=*/0);
            stage = sbe::makeS<sbe::LimitSkipStage>(
                std::move(stage),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           sbe::value::bitcastFrom<int64_t>(spec.sampleSize)),
                /*skip=*/nullptr,
                /*nodeId=*/0);
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }

    maybeInjectProjection(spec, ids, stage, staticData);

    stage_builder::PlanStageData data{
        stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
        std::move(staticData)};
    return {std::move(stage), std::move(data)};
}

/**
 * Builds the sampling plan for 'technique'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> makeSamplingPlan(
    const SampleSpec& spec,
    const CollectionPtr& collection,
    OperationContext* opCtx,
    SamplingTechniqueEnum technique,
    PlanYieldPolicySBE* sbeYieldPolicy) {
    switch (technique) {
        case SamplingTechniqueEnum::kRandom:
            return makeRandomSamplingPlan(spec, collection, sbeYieldPolicy);
        case SamplingTechniqueEnum::kChunk:
            tassert(9372901,
                    "The number of chunks should be positive.",
                    spec.numChunks && *spec.numChunks > 0);
            return makeChunkSamplingPlan(spec, collection, sbeYieldPolicy);
        case SamplingTechniqueEnum::kFullCollScan:
            return makeFullCollScanPlan(spec, collection, sbeYieldPolicy);
        case SamplingTechniqueEnum::kSeqScan:
        case SamplingTechniqueEnum::kStrides:
            return makeTestSamplingPlan(spec, collection, opCtx, technique, sbeYieldPolicy);
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

std::unique_ptr<OnTheFlySampleLoader> makeOnTheFlySampleLoader(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
    const SampleSpec& spec) {
    const size_t collectionCard =
        static_cast<size_t>(collections.lookupCollection(spec.nss)->numRecords(opCtx));
    return std::make_unique<OnTheFlySampleLoader>(opCtx,
                                                  collections,
                                                  yieldPolicy,
                                                  std::move(customerQueryExpCtx),
                                                  resolveSamplingTechnique(spec, collectionCard));
}

OnTheFlySampleLoader::OnTheFlySampleLoader(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::intrusive_ptr<const ExpressionContext> customerQueryExpCtx,
    SamplingTechniqueEnum technique)
    : SampleLoader(opCtx, collections, yieldPolicy, std::move(customerQueryExpCtx)),
      _technique(technique) {}

StatusWith<Sample> OnTheFlySampleLoader::tryLoad(const SampleSpec& spec) {
    auto cq = makeEmptyCanonicalQuery(spec.nss, _opCtx, _customerQueryExpCtx);
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, spec.nss);
    auto plan = makeSamplingPlan(
        spec, _collections.lookupCollection(spec.nss), _opCtx, _technique, sbeYieldPolicy.get());

    return executeSamplingQueryAndSample(
        _opCtx, _collections, spec.nss, plan, std::move(cq), std::move(sbeYieldPolicy), _technique);
}

}  // namespace mongo::ce
