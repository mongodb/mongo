// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/sbe_single_document_lookup_executor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_util.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_parameterization.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

using namespace std::literals::string_view_literals;

namespace mongo::exec::agg {
namespace {

// Builds the CanonicalQuery once for a lookup against 'nss'.
std::unique_ptr<CanonicalQuery> buildCanonicalQuery(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const BSONObj& filter,
                                                    const CollatorInterface* defaultCollator) {
    auto findCmd = std::make_unique<FindCommandRequest>(nss);
    findCmd->setFilter(filter);

    // Clear the IDHACK flag that ExpressionContextBuilder auto-sets for an '{_id: X}' filter, so
    // the planner enumerates indexes (and produces the index-aware SBE plan).
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx =
            ExpressionContextBuilder{}.fromRequest(opCtx, *findCmd).isIdHackQuery(false).build(),
        .parsedFind = ParsedFindCommandParams{std::move(findCmd)},
    });

    // The find command carries no collation, so adopt the collection's default collation.
    if (defaultCollator) {
        cq->setCollator(defaultCollator->clone());
    }

    // Parameterize the match expression so the SBE bounds builder assigns stable InputParamIds to
    // each comparison leaf; SlotBinder::bind later rebinds those slots per documentKey.
    auto inputParamIdToExpression = parameterizeMatchExpression(cq->getPrimaryMatchExpression());
    cq->addMatchParams(inputParamIdToExpression);
    return cq;
}

// Describes to the eligibility what a freshly-acquired collection handle should be reported as
// held, so later lookups within the same window can skip routing. A sharded collection still needs
// an ownership check against its pinned filter. Both unsharded collections (untracked and
// unsplittable) are local by construction once held, so they collapse to
// HeldUnshardedCollectionLocally.
LocalLookupEligibility::AcquisitionState acquisitionStateFor(
    const CollectionAcquirer::Handle& collectionAcquisition) {
    const auto& acquisition = collectionAcquisition.collection();
    if (acquisition.getShardingDescription().isSharded()) {
        const auto& filter = acquisition.getShardingFilter();
        tassert(
            12952809, "sharded acquisition is missing its ownership filter", filter.has_value());
        return LocalLookupEligibility::HeldShardedCollection{*filter};
    }
    return LocalLookupEligibility::HeldUnshardedCollectionLocally{};
}

bool isExecutorInvalidated(const CollectionAcquirer::Handle& coll,
                           const UUID& planUUID,
                           size_t planVersion) {
    if (!coll.exists()) {
        return true;
    }
    const auto& collPtr = coll.getCollectionPtr();
    return planUUID != coll.uuid() ||
        planVersion != CollectionQueryInfo::get(collPtr).getPlanCacheInvalidatorVersion();
}

}  // namespace

// --- SlotBinder ---

boost::optional<SbeSingleDocumentLookupExecutor::SlotBinder>
SbeSingleDocumentLookupExecutor::SlotBinder::make(const stage_builder::PlanStageData& data) {
    const auto& staticData = *data.staticData;
    const bool isClustered = !staticData.clusteredCollBoundsInfos.empty();
    const bool isIxscan = !staticData.indexBoundsEvaluationInfos.empty();

    // Exactly one of the two bound forms must be present. Mixing or absence is a shape we don't
    // recognize. The _id-only filter we feed the planner produces exactly one of these.
    if (isClustered == isIxscan) {
        return boost::none;
    }

    // Clustered collection. Bounded STAGE_COLLSCAN with min/max RecordId slots. Cluster
    // key is the _id field; the documentKey value may be scalar or compound BSON.
    if (isClustered) {
        if (staticData.clusteredCollBoundsInfos.size() != 1) {
            return boost::none;
        }
        const auto& info = staticData.clusteredCollBoundsInfos.front();
        if (!info.minRecord || !info.maxRecord) {
            return boost::none;
        }
        if (staticData.clusterKeyFieldName != "_id"sv) {
            return boost::none;
        }

        // A non-simple collation on the clustered key means record_id_helpers::keyForElem below
        // would encode raw bytes rather than collation keys, producing wrong seek bounds for
        // string _ids. Decline so the caller falls back, which builds bounds through the
        // collation-aware path.
        if (staticData.ccCollator) {
            return boost::none;
        }

        return SlotBinder{
            .kind = Kind::kClusteredRecordIdPair,
            .slotA = *info.minRecord,
            .slotB = *info.maxRecord,
        };
    }

    // Non-clustered collection. _id_ IXSCAN with a SingleIntervalPlan slot pair.
    if (staticData.indexBoundsEvaluationInfos.size() != 1) {
        return boost::none;
    }
    const auto& info = staticData.indexBoundsEvaluationInfos.front();
    if (info.index.keyPattern.nFields() != 1) {
        return boost::none;
    }
    if (!std::holds_alternative<stage_builder::ParameterizedIndexScanSlots::SingleIntervalPlan>(
            info.slots.slots)) {
        return boost::none;
    }
    const auto& single =
        std::get<stage_builder::ParameterizedIndexScanSlots::SingleIntervalPlan>(info.slots.slots);

    auto keyField = info.index.keyPattern.firstElement().fieldNameStringData();
    if (keyField != "_id"sv) {
        return boost::none;
    }

    return SlotBinder{
        .kind = Kind::kIxscanKeyPair,
        .slotA = single.lowKey,
        .slotB = single.highKey,
        .ksVersion = info.keyStringVersion,
        .ordering = info.ordering,
        .direction = info.direction,
        .collator = info.index.collator,
    };
}

bool SbeSingleDocumentLookupExecutor::SlotBinder::bind(const BSONElement& idElem,
                                                       sbe::RuntimeEnvironment* env) const {
    switch (kind) {
        case Kind::kIxscanKeyPair: {
            if (MONGO_unlikely(
                    !IndexBoundsBuilder::isDirectlyEncodableEqualityType(idElem.type()))) {
                // Non-scalar literal (regex, array, undefined) would widen the bounds beyond a
                // single point. documentKey._id is never these in practice; refuse rather than
                // produce wrong bounds.
                return false;
            }

            // Encode collation comparison keys when the _id index is collation-aware (no-op for a
            // simple index: appends the value as-is). This makes the seek bounds match the index's
            // key encoding, mirroring the classic/Express _id path.
            BSONObjBuilder keyBob;
            CollationIndexKey::collationAwareIndexKeyAppend(idElem, collator, &keyBob);
            BSONObj key = keyBob.obj();
            auto [lowKs, highKs] = stage_builder::makeKeyStringPair(key,
                                                                    /*lowKeyInclusive=*/true,
                                                                    key,
                                                                    /*highKeyInclusive=*/true,
                                                                    ksVersion,
                                                                    ordering,
                                                                    /*forward=*/direction == 1);
            env->resetSlot(slotA,
                           sbe::value::TypeTags::keyString,
                           sbe::value::makeKeyString(std::move(lowKs)).second,
                           /*owned=*/true);
            env->resetSlot(slotB,
                           sbe::value::TypeTags::keyString,
                           sbe::value::makeKeyString(std::move(highKs)).second,
                           /*owned=*/true);
            return true;
        }
        case Kind::kClusteredRecordIdPair: {
            // record_id_helpers::keyForElem handles both scalar and compound BSON _id (e.g.
            // ChangeStreamPreImageId). For a point seek the min and max RecordIds are equal.
            auto rid = record_id_helpers::keyForElem(idElem);
            auto [minTag, minVal] = sbe::value::makeCopyRecordId(rid);
            env->resetSlot(slotA, minTag, minVal, /*owned=*/true);
            auto [maxTag, maxVal] = sbe::value::makeCopyRecordId(rid);
            env->resetSlot(slotB, maxTag, maxVal, /*owned=*/true);
            return true;
        }
    }
    MONGO_UNREACHABLE_TASSERT(12952806);
}

// --- PreparedExecutor ---

boost::optional<SbeSingleDocumentLookupExecutor::PreparedExecutor>
SbeSingleDocumentLookupExecutor::PreparedExecutor::make(OperationContext* opCtx,
                                                        const CollectionAcquirer::Handle& coll,
                                                        std::unique_ptr<CanonicalQuery> cq,
                                                        bool shouldApplyShardFilter) {
    const NamespaceString& nss = coll.nss();
    MultipleCollectionAccessor collections(coll.collection());

    // Request a shard-filter stage so orphans physically present on this node are dropped, unless
    // the eligibility already resolved ownership from the shard key. Gate on isSharded() the same
    // way get_executor does: requiresShardFiltering() calls getShardKeyPattern(), which invariants
    // on an unsharded collection. When set, the shardFilterer slot is populated from the
    // acquisition by prepareSlotBasedExecutableTree() below.
    const bool applyShardFilter =
        shouldApplyShardFilter && coll.collection().getShardingDescription().isSharded();
    const size_t plannerOptions =
        applyShardFilter ? QueryPlannerParams::INCLUDE_SHARD_FILTER : QueryPlannerParams::DEFAULT;
    auto plannerParams = QueryPlannerParams(QueryPlannerParams::ArgsForSingleCollectionQuery{
        .opCtx = opCtx,
        .canonicalQuery = *cq,
        .collections = collections,
        .plannerOptions = plannerOptions,
    });
    auto solutions = uassertStatusOK(QueryPlanner::plan(*cq, plannerParams));
    if (solutions.empty()) {
        return boost::none;
    }

    // INTERRUPT_ONLY: the plan stays interruptible but never yields on its own. The acquisition is
    // owned by the caller and dropped only by releaseResources() call.
    auto yieldPolicy = PlanYieldPolicySBE::make(
        opCtx, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY, collections, nss);

    auto [root, data] = stage_builder::buildSlotBasedExecutableTree(
        opCtx, collections, *cq, *solutions[0], yieldPolicy.get());

    stage_builder::prepareSlotBasedExecutableTree(opCtx,
                                                  root.get(),
                                                  &data,
                                                  *cq,
                                                  collections,
                                                  yieldPolicy.get(),
                                                  /*preparingFromCache=*/false);

    // Compute the binder. Return boost::none (without constructing anything) on an unsupported plan
    // shape so the caller can fall back cleanly.
    auto binder = SlotBinder::make(data);
    if (!binder) {
        return boost::none;
    }

    // Park the freshly-built plan in the SBE "saved" state. prepareSlotBasedExecutableTree() leaves
    // it runnable.
    root->saveState();

    return PreparedExecutor{
        std::move(root),
        std::move(data),
        std::move(yieldPolicy),
        std::move(cq),
        coll.uuid(),
        CollectionQueryInfo::get(coll.getCollectionPtr()).getPlanCacheInvalidatorVersion(),
        std::move(*binder),
    };
}

// --- SbeSingleDocumentLookupExecutor ---

SingleDocumentLookupExecutor::LookupResult SbeSingleDocumentLookupExecutor::performLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<Timestamp> afterClusterTime) {
    OperationContext* opCtx = expCtx->getOperationContext();

    // Timed around the whole eligibility run (not just the terminal attempt) so the recorded
    // latency reflects what the caller actually waited, including any StaleConfig-triggered
    // routing refresh + retry.
    Timer timer;
    LookupResult outcome = _localEligibility->run(
        expCtx,
        nss,
        documentKey,
        _acquisitionState,
        [&](const LocalLookupEligibility::Decision& decision) -> LookupResult {
            // The document does not live on the local shard. Early exit.
            if (!LocalLookupEligibility::isLocal(decision)) {
                return {LookupResult::HandledStatus::kNotHandled, boost::none};
            }

            // Single cleanup point: in case of unexpected exit (e.g. exception) drops all cached
            // state so the next lookup rebuilds.
            ScopeGuard resetOnException([&] { resetCachedState(); });

            // The planner sees an _id-only filter even when the documentKey carries shard-key
            // fields too: _id is unique per shard and the lookup runs locally, so it returns
            // exactly the right document.
            const auto idFilter = makeIdEqualityFilter(documentKey);
            if (!idFilter) {
                LOGV2_DEBUG(12952803,
                            1,
                            "SbeSingleDocumentLookupExecutor: documentKey has no _id, falling back",
                            "documentKey"_attr = redact(documentKey.toBson()));
                return {LookupResult::HandledStatus::kNotHandled, boost::none};
            }

            const auto& local = std::get<LocalLookupEligibility::Local>(decision);
            auto shardRoleScope = createScopedShardRole(opCtx, nss, local);
            return withCollectionGoneMappedToNotFound([&]() -> LookupResult {
                const CollectionAcquirer::Handle& coll =
                    getOrAcquireCollection(opCtx, nss, collectionUUID);
                if (!coll.exists()) {
                    // Collection doesn't exist. Early exit as no document can be found.
                    return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
                }
                assertLocalLookupReadAtOrAfter(opCtx, afterClusterTime);

                if (!getOrMakeExecutor(
                        opCtx, coll, nss, *idFilter, !_eligibilityChecksShardKeyOwnership)) {
                    // Planning failed or plan shape is not one SlotBinder supports.
                    return {LookupResult::HandledStatus::kNotHandled, boost::none};
                }

                // Rebind the cached executor's bounds slots to this _id. A false return means the
                // _id value type can't be expressed as a single bound pair. This is a per-document
                // encoding failure, not a plan defect. Dismiss the ScopeGuard so the cached plan
                // survives for the next event in the same window.
                if (!_preparedExecutor->slotBinder.bind(idFilter->firstElement(),
                                                        _preparedExecutor->data.env.runtimeEnv)) {
                    resetOnException.dismiss();
                    return {LookupResult::HandledStatus::kNotHandled, boost::none};
                }

                // Publish this lookup to $indexStats. The executor (hence the index used) is known
                // statically, so this needs no runtime stats walk. Keys/docs examined for the sink
                // are folded in separately by writePlanSummaryStats() at teardown.
                auto result = executeOnce(opCtx);
                recordIndexUsage(
                    coll.getCollectionPtr().get(), indexUsageSummary(), _planSummaryStatsSink);
                resetOnException.dismiss();
                return result;
            });
        });

    if (_recorder) {
        switch (outcome.status) {
            case LookupResult::HandledStatus::kDocumentFound:
                _recorder->recordFound(timer.elapsed());
                break;
            case LookupResult::HandledStatus::kDocumentNotFound:
                _recorder->recordNotFound(timer.elapsed());
                break;
            case LookupResult::HandledStatus::kNotHandled:
                _recorder->recordNotHandled();
                break;
        }
    }
    return outcome;
}

CollectionAcquirer::Handle& SbeSingleDocumentLookupExecutor::getOrAcquireCollection(
    OperationContext* opCtx, const NamespaceString& nss, boost::optional<UUID> uuid) {
    // Acquire on the first call and reuse afterwards.
    if (!_collectionAcquisition) {
        _collectionAcquisition.emplace(_collectionAcquirer->acquireCollection(opCtx, nss, uuid));
        _acquisitionState = acquisitionStateFor(*_collectionAcquisition);
        return *_collectionAcquisition;
    }

    // The cached acquisition can outlive a DDL rename+recreate that gives the target nss a fresh
    // UUID. Check for UUID mismatch and throw an exception.
    if (_collectionAcquisition->exists() && uuid) {
        checkCollectionUUIDMismatch(opCtx, nss, _collectionAcquisition->getCollectionPtr(), *uuid);
    }
    return *_collectionAcquisition;
}

bool SbeSingleDocumentLookupExecutor::getOrMakeExecutor(OperationContext* opCtx,
                                                        const CollectionAcquirer::Handle& coll,
                                                        const NamespaceString& nss,
                                                        const BSONObj& filter,
                                                        bool shouldApplyShardFilter) {
    // First call (or invalidation): build the CanonicalQuery and create the SBE executor once.
    // 'filter' seeds the planner with literal values to generate bounds + IETs from. On subsequent
    // calls the cached executor is reused. SlotBinder::bind() resets the parameterized slots at
    // the current documentKey.
    if (!_preparedExecutor ||
        isExecutorInvalidated(
            coll, _preparedExecutor->collectionUUID, _preparedExecutor->collectionVersion)) {
        resetPlan();
        _preparedExecutor = PreparedExecutor::make(
            opCtx,
            coll,
            buildCanonicalQuery(opCtx, nss, filter, coll.getCollectionPtr()->getDefaultCollator()),
            shouldApplyShardFilter);
        if (!_preparedExecutor) {
            return false;
        }
        ++_planRebuildCount_forTest;
    }
    return true;
}

SingleDocumentLookupExecutor::LookupResult SbeSingleDocumentLookupExecutor::executeOnce(
    OperationContext* opCtx) {
    using PlanState = PreparedExecutor::PlanState;

    const bool reOpen = (_preparedExecutor->planState != PlanState::kUnopened);
    if (_preparedExecutor->planState != PlanState::kActive) {
        _preparedExecutor->root->restoreState();
    }
    _preparedExecutor->planState = PlanState::kActive;

    auto& root = _preparedExecutor->root;
    root->open(reOpen);
    auto state = root->getNext();
    if (state != sbe::PlanState::ADVANCED) {
        return {LookupResult::HandledStatus::kDocumentNotFound, boost::none};
    }

    // The executor returns the whole document, so the result slot holds a BSONObj. Copy it owned.
    tassert(12952807,
            "SBE plan must have a result slot",
            _preparedExecutor->data.staticData->resultSlot);
    auto* resultAccessor = root->getAccessor(_preparedExecutor->data.env.ctx,
                                             *_preparedExecutor->data.staticData->resultSlot);
    auto [resultTag, resultVal] = resultAccessor->getViewOfValue();
    tassert(12952808,
            "SBE lookup result slot must hold a BSONObj",
            resultTag == sbe::value::TypeTags::bsonObject);
    return {LookupResult::HandledStatus::kDocumentFound,
            Document(BSONObj(sbe::value::bitcastTo<const char*>(resultVal)).getOwned())};
}

void SbeSingleDocumentLookupExecutor::releaseResources() noexcept {
    // Reset the state. It will be rebuilt again on the first performLookup() call.
    resetCachedState();
}

PlanSummaryStats SbeSingleDocumentLookupExecutor::indexUsageSummary() const {
    PlanSummaryStats summary;
    // An _id ixscan is the only shape that uses a tracked index. A clustered bounded _id scan is
    // reported as EXPRESS_CLUSTERED_IXSCAN with no index and no collection scan on the Express
    // path, and a clustered collection has no _id_ index registered in the usage tracker at all, so
    // leave the summary empty to match Express and avoid recording a phantom collection scan.
    if (_preparedExecutor->slotBinder.kind == SlotBinder::Kind::kIxscanKeyPair) {
        summary.indexesUsed = {"_id_"};
    }
    return summary;
}

void SbeSingleDocumentLookupExecutor::writePlanSummaryStats() noexcept {
    if (!_planSummaryStatsSink || !_preparedExecutor) {
        return;
    }
    PlanSummaryStats summary;
    PlanSummaryStatsVisitor visitor(summary);
    _preparedExecutor->root->accumulate(kEmptyPlanNodeId, &visitor);
    _planSummaryStatsSink->totalKeysExamined +=
        summary.totalKeysExamined - _preparedExecutor->lastAccumulatedKeysExamined;
    _planSummaryStatsSink->totalDocsExamined +=
        summary.totalDocsExamined - _preparedExecutor->lastAccumulatedDocsExamined;
    _preparedExecutor->lastAccumulatedKeysExamined = summary.totalKeysExamined;
    _preparedExecutor->lastAccumulatedDocsExamined = summary.totalDocsExamined;
}

void SbeSingleDocumentLookupExecutor::resetPlan() noexcept {
    // Write the plan summary stats into the '_planSummaryStatsSink' before destroying the executor.
    writePlanSummaryStats();
    _preparedExecutor.reset();
}

void SbeSingleDocumentLookupExecutor::resetAcquisition() noexcept {
    // Order matters: a sharded '_acquisitionState' holds a reference into the acquisition's
    // ownership filter, so it must be cleared before the acquisition it points into is destroyed.
    _acquisitionState = LocalLookupEligibility::NoHeldAcquisition{};
    _collectionAcquisition.reset();
}

void SbeSingleDocumentLookupExecutor::resetCachedState() noexcept {
    // noexcept because it runs inside the performLookup ScopeGuard during exception unwinding.
    resetPlan();
    resetAcquisition();
}

}  // namespace mongo::exec::agg
