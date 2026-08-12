// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/stage_builder/sbe/gen_coll_scan.h"

#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_filter.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
namespace {

void openCallback(OperationContext* opCtx, const CollectionPtr& collection) {
    // Forward, non-tailable scans from the oplog need to wait until all oplog entries
    // before the read begins to be visible. This isn't needed for reverse scans because
    // we only hide oplog entries from forward scans, and it isn't necessary for tailing
    // cursors because they ignore EOF and will eventually see all writes. Forward,
    // non-tailable scans are the only case where a meaningful EOF will be seen that
    // might not include writes that finished before the read started. This also must be
    // done before we create the cursor as that is when we establish the endpoint for
    // the cursor. Also call abandonSnapshot to make sure that we are using a fresh
    // storage engine snapshot while waiting. Otherwise, we will end up reading from the
    // snapshot where the oplog entries are not yet visible even after the wait.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx, collection->getRecordStore());
}

/**
 * Checks whether a callback function should be created for a ScanStage and returns it, if so. The
 * logic in the provided callback will be executed when the ScanStage is opened (but not reopened).
 */
sbe::ScanOpenCallback makeOpenCallbackIfNeeded(const CollectionPtr& collection,
                                               const CollectionScanNode* csn) {
    if (csn->direction == CollectionScanParams::FORWARD && csn->shouldWaitForOplogVisibility) {
        tassert(7714200, "Expected 'tailable' to be false", !csn->tailable);
        tassert(7714201, "Expected 'collection' to be the oplog", collection->ns().isOplog());

        return &openCallback;
    } else {
        return nullptr;
    }
}

// Unlike generateGenericCollScan() below, this takes 'scanFieldNames' by value: it does not add the
// filter's dependencies to the list, so the caller's copy still describes exactly the fields the
// scan produces. The consequence is that a field referenced only by the filter gets no slot here,
// and the filter reads it from the result object instead. That is a missed optimization rather than
// a correctness issue.
// TODO(SERVER-133309): Consider extracting the filter's top-level fields into slots here too.
SbBuilder::MakeScanResult generateClusteredCollScan(SbBuilder& b,
                                                    StageBuilderState& state,
                                                    const CollectionPtr& collection,
                                                    bool forward,
                                                    RecordIdRange range,
                                                    std::vector<std::string> scanFieldNames) {
    SbScanBounds scanBounds;
    if (range.getMin()) {
        auto [tag, val] = sbe::value::makeCopyRecordId(range.getMin()->recordId());
        scanBounds.minRecordIdSlot =
            SbSlot{state.env->registerSlot(tag, val, true, state.slotIdGenerator)};
    }
    if (range.getMax()) {
        auto [tag, val] = sbe::value::makeCopyRecordId(range.getMax()->recordId());
        scanBounds.maxRecordIdSlot =
            SbSlot{state.env->registerSlot(tag, val, true, state.slotIdGenerator)};
    }
    scanBounds.includeScanStartRecordId = forward ? range.isMinInclusive() : range.isMaxInclusive();
    scanBounds.includeScanEndRecordId = forward ? range.isMaxInclusive() : range.isMinInclusive();

    state.data->clusteredCollBoundsInfos.emplace_back(ParameterizedClusteredScanSlots{
        b.lower(scanBounds.minRecordIdSlot), b.lower(scanBounds.maxRecordIdSlot)});

    return b.makeScan(collection->uuid(),
                      collection->ns().dbName(),
                      forward,
                      std::move(scanFieldNames),
                      std::move(scanBounds));
}  // generateClusteredCollScan

/**
 * Generates a generic collection scan sub-tree.
 * Note that 'fields' is an in/out parameter: it is augmented with the top-level fields referenced
 * by 'csn->filter' below, and the caller relies on seeing those additions so that it can map the
 * scan's field slots into 'PlanStageSlots'. Otherwise the filter would have no kField slot to use
 * and would redundantly re-extract the field from the result object.
 */
SbBuilder::MakeScanResult generateGenericCollScan(SbBuilder& b,
                                                  StageBuilderState& state,
                                                  const CollectionPtr& collection,
                                                  const CollectionScanNode* csn,
                                                  std::vector<std::string>& fields) {
    const bool forward = csn->direction == CollectionScanParams::FORWARD;

    tassert(9884951, "resumeScanPoint not supported in SBE", !csn->resumeScanPoint);

    tassert(9884952,
            "SBE does not support queries on the oplog",
            !collection->ns().isOplog() && !csn->shouldTrackLatestOplogTimestamp);

    if (csn->filter) {
        DepsTracker deps;
        dependency_analysis::addDependencies(csn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            auto topLevelFields = getTopLevelFields(deps.fields);
            fields = appendVectorUnique(std::move(fields), std::move(topLevelFields));
        }
    }

    sbe::ScanOpenCallback callback = makeOpenCallbackIfNeeded(collection, csn);

    boost::optional<SbSlot> oplogTsSlot;

    return b.makeScan(collection->uuid(),
                      collection->ns().dbName(),
                      forward,
                      fields,
                      SbScanBounds{},
                      SbIndexInfoSlots{},
                      std::move(callback),
                      oplogTsSlot);
}  // generateGenericCollScan

}  // namespace

std::pair<SbStage, PlanStageSlots> generateCollScan(StageBuilderState& state,
                                                    const CollectionPtr& collection,
                                                    const CollectionScanNode* csn,
                                                    std::vector<std::string> fields) {
    // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
    tassert(11051827,
            "Unexpected stopApplyingFilterAfterFirstMatch flag in non-oplog scan",
            !csn->stopApplyingFilterAfterFirstMatch);

    tassert(9884961, "resumeScanPoint not supported in SBE", !csn->resumeScanPoint);

    SbBuilder b(state, csn->nodeId());

    // Empty range list (∅) — nothing can match, return an empty limit stage.
    // TODO(SERVER-133100): Handle trivially empty scans at the QO level instead.
    // Then the branch below can be replaced with a tassert.
    if (csn->rangeList.isEmpty()) {
        PlanStageReqs eofReqs;
        eofReqs.setResultObj().set(PlanStageSlots::kRecordId);
        PlanStageSlots outputs;
        outputs.setMissingRequiredNamedSlotsToNothing(state, eofReqs);
        return {b.makeLimit(b.makeCoScan(), b.makeInt64Constant(0)), std::move(outputs)};
    }

    SbStage stage;
    SbSlot resultSlot;
    SbSlot recordIdSlot;
    SbSlotVector scanFieldSlots;

    if (csn->doClusteredCollectionScanSbe()) {
        // The minRecord and maxRecord optimizations are not compatible with resume tokens.
        tassert(9884902,
                "'resumeScanPoint' cannot be used with 'rangeList' bounds",
                !(csn->resumeScanPoint && !csn->rangeList.isUnbounded()));

        const bool forward = csn->direction == CollectionScanParams::FORWARD;

        if (csn->rangeList.getRanges().size() == 1) {
            auto range = csn->rangeList.outerBounds();
            std::tie(stage, resultSlot, recordIdSlot, scanFieldSlots) =
                generateClusteredCollScan(b, state, collection, forward, std::move(range), fields);
        } else {
            std::tie(stage, resultSlot, recordIdSlot, scanFieldSlots) = b.makeScan(
                collection->uuid(), collection->ns().dbName(), forward, fields, csn->rangeList);
        }
    } else {
        std::tie(stage, resultSlot, recordIdSlot, scanFieldSlots) =
            generateGenericCollScan(b, state, collection, csn, fields);
    }

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < fields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), scanFieldSlots[i]);
    }

    if (csn->filter) {
        SbExpr filterExpr =
            generateFilter(state,
                           csn->filter.get(),
                           resultSlot,
                           outputs,
                           /*isFilterOverIxscan*/ false,
                           /*canUsePathArrayness*/
                           state.expCtx->getQueryKnobConfiguration().getEnablePathArrayness());
        if (!filterExpr.isNull()) {
            stage = b.makeFilter(std::move(stage), std::move(filterExpr));
        }
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
