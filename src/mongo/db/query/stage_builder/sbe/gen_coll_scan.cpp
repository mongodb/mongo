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


#include "mongo/db/query/stage_builder/sbe/gen_coll_scan.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_filter.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
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

/**
 * Creates a collection scan sub-tree optimized for clustered collection scans. Should only be
 * called on clustered collections. We can build an optimized scan when any of the following
 * scenarios apply:
 *
 * 1. 'csn->minRecord' and/or 'csn->maxRecord' exist.
 *    1.1 CollectionScanParams::FORWARD scan:
 *        a. If 'csn->minRecord' is present, the collection scan will seek directly to the RecordId
 *           of a record as close to this lower bound as possible without going higher.
 *        b. If 'csn->maxRecord' is present, the collection scan will stop and return EOF the first
 *           time it fetches a document greater than this upper bound.
 *    1.2 CollectionScanParams::BACKWARD scan:
 *        a. If 'csn->maxRecord' is present, the collection scan will seek directly to the RecordId
 *           of a record as close to this upper bound as possible without going lower.
 *        b. If 'csn->minRecord' is present, the collection scan will stop and return EOF the first
 *           time it fetches a document less than this lower bound.
 * 2. The user request specified a $_resumeAfter RecordId from which to begin the scan AND the scan
 *    is forward AND neither 'csn->minRecord' nor 'csn->maxRecord' exist.
 *    2a. The scan will continue with the next RecordId after $_resumeAfter.
 */
std::pair<SbStage, PlanStageSlots> generateClusteredCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::vector<std::string> scanFieldNames) {
    SbBuilder b(state, csn->nodeId());

    const bool forward = csn->direction == CollectionScanParams::FORWARD;

    invariant(csn->doClusteredCollectionScanSbe());

    tassert(9884961, "resumeScanPoint not supported in SBE", !csn->resumeScanPoint);

    tassert(9884962,
            "SBE does not support queries on the oplog",
            !collection->ns().isOplog() && !csn->shouldTrackLatestOplogTimestamp);

    // The minRecord and maxRecord optimizations are not compatible with resume tokens.
    tassert(9884902,
            "'resumeScanPoint' cannot be used with 'minRecord' or 'maxRecord'",
            !(csn->resumeScanPoint && (csn->minRecord || csn->maxRecord)));
    // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
    tassert(9884903,
            "Cannot use 'stopApplyingFilterAfterFirstMatch' when generating clustered scan",
            !csn->stopApplyingFilterAfterFirstMatch);

    SbStage resumeRecordIdTree;
    boost::optional<SbSlot> seekSlot;

    // Create minRecordId and/or maxRecordId slots as needed.
    boost::optional<SbSlot> minRecordSlot;
    boost::optional<SbSlot> maxRecordSlot;
    if (csn->minRecord) {
        auto [tag, val] = sbe::value::makeCopyRecordId(csn->minRecord->recordId());
        minRecordSlot = SbSlot{state.env->registerSlot(tag, val, true, state.slotIdGenerator)};
    }
    if (csn->maxRecord) {
        auto [tag, val] = sbe::value::makeCopyRecordId(csn->maxRecord->recordId());
        maxRecordSlot = SbSlot{state.env->registerSlot(tag, val, true, state.slotIdGenerator)};
    }
    state.data->clusteredCollBoundsInfos.emplace_back(
        ParameterizedClusteredScanSlots{b.lower(minRecordSlot), b.lower(maxRecordSlot)});

    // Create the ScanStage.
    bool includeScanStartRecordId =
        (csn->boundInclusion ==
             CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords ||
         csn->boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly);
    bool includeScanEndRecordId =
        (csn->boundInclusion ==
             CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords ||
         csn->boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly);

    SbScanBounds scanBounds;
    scanBounds.minRecordIdSlot = minRecordSlot;
    scanBounds.maxRecordIdSlot = maxRecordSlot;
    scanBounds.includeScanStartRecordId = includeScanStartRecordId;
    scanBounds.includeScanEndRecordId = includeScanEndRecordId;

    auto [stage, resultSlot, recordIdSlot, scanFieldSlots] =
        b.makeScan(collection->uuid(),
                   collection->ns().dbName(),
                   forward,
                   seekSlot,
                   scanFieldNames,  // do not std::move - used later
                   std::move(scanBounds));

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, scanFieldNames[i]), scanFieldSlots[i]);
    }

    // When the start and/or end scan bounds are from an expression, ScanStage::getNext() treats
    // them both as inclusive, and 'csn->filter' will enforce any exclusions. If the bound(s) came
    // from the "min" (always inclusive) and/or "max" (always exclusive) keywords, there may be no
    // filter, so ScanStage->getNext() must directly enforce the bounds. min's inclusivity matches
    // getNext()'s default behavior, but max's exclusivity does not and thus is enforced by the
    // includeScanEndRecordId argument to the ScanStage constructor above.
    SbExpr filterExpr = generateFilter(state, csn->filter.get(), resultSlot, outputs);
    if (!filterExpr.isNull()) {
        stage = b.makeFilter(std::move(stage), std::move(filterExpr));
    }

    return {std::move(stage), std::move(outputs)};
}  // generateClusteredCollScan

/**
 * Generates a generic collection scan sub-tree.
 */
std::pair<SbStage, PlanStageSlots> generateGenericCollScan(StageBuilderState& state,
                                                           const CollectionPtr& collection,
                                                           const CollectionScanNode* csn,
                                                           std::vector<std::string> fields) {
    SbBuilder b(state, csn->nodeId());

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

    sbe::ScanCallbacks callbacks({}, {}, makeOpenCallbackIfNeeded(collection, csn));

    SbStage resumeRecordIdTree;
    boost::optional<SbSlot> seekSlot;

    boost::optional<SbSlot> oplogTsSlot;

    auto [stage, resultSlot, recordIdSlot, fieldSlots] = b.makeScan(collection->uuid(),
                                                                    collection->ns().dbName(),
                                                                    forward,
                                                                    seekSlot,
                                                                    fields,
                                                                    SbScanBounds{},
                                                                    SbIndexInfoSlots{},
                                                                    std::move(callbacks),
                                                                    oplogTsSlot);

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < fields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), fieldSlots[i]);
    }

    if (csn->filter) {
        // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        auto filterExpr = generateFilter(state, csn->filter.get(), resultSlot, outputs);
        if (!filterExpr.isNull()) {
            stage = b.makeFilter(std::move(stage), std::move(filterExpr));
        }
    }

    return {std::move(stage), std::move(outputs)};
}  // generateGenericCollScan

}  // namespace

std::pair<SbStage, PlanStageSlots> generateCollScan(StageBuilderState& state,
                                                    const CollectionPtr& collection,
                                                    const CollectionScanNode* csn,
                                                    std::vector<std::string> fields) {
    if (csn->doClusteredCollectionScanSbe()) {
        return generateClusteredCollScan(state, collection, csn, std::move(fields));
    } else {
        return generateGenericCollScan(state, collection, csn, std::move(fields));
    }
}
}  // namespace mongo::stage_builder
