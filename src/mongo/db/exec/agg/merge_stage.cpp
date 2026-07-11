// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/merge_stage.h"

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceMergeToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto ds = boost::dynamic_pointer_cast<DocumentSourceMerge>(documentSource);

    tassert(10561500, "expected 'DocumentSourceMerge' type", ds);

    return make_intrusive<exec::agg::MergeStage>(ds->kStageName,
                                                 ds->getExpCtx(),
                                                 ds->getOutputNs(),
                                                 ds->_mergeOnFields,
                                                 ds->_mergeOnFieldsIncludesId,
                                                 ds->_mergeProcessor);
}

namespace exec::agg {

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceMergeBatch);
REGISTER_AGG_STAGE_MAPPING(mergeStage, DocumentSourceMerge::id, documentSourceMergeToStageFn)


std::pair<MergeStage::BatchObject, int> MergeStage::makeBatchObject(Document doc) const {
    auto batchObject =
        _mergeProcessor->makeBatchObject(std::move(doc), *_mergeOnFields, _mergeOnFieldsIncludesId);
    auto upsertType = _mergeProcessor->getMergeStrategyDescriptor().upsertType;

    tassert(6628901, "_writeSizeEstimator should be initialized", _writeSizeEstimator);
    int size = _writeSizeEstimator->estimateUpdateSizeBytes(batchObject, upsertType);
    return {std::move(batchObject), size};
}

bool MergeStage::shouldFlush(size_t currentBatchSize) const {
    return _mergeProcessor->shouldFlush(currentBatchSize);
}

void MergeStage::flush(BatchedCommandRequest bcr, BatchedObjects batch) {
    try {
        DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
        _mergeProcessor->flush(_outputNs, *_mergeOnFields, std::move(bcr), std::move(batch));
    } catch (const ExceptionFor<ErrorCodes::ImmutableField>& ex) {
        uassertStatusOKWithContext(ex.toStatus(),
                                   "$merge failed to update the matching document, did you "
                                   "attempt to modify the _id or the shard key?");
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
        // A DuplicateKey error could be due to a collision on the 'on' fields or on any other
        // unique index.
        auto dupKeyPattern = ex->getKeyPattern();
        bool dupKeyFromMatchingOnFields =
            (static_cast<size_t>(dupKeyPattern.nFields()) == _mergeOnFields->size()) &&
            std::all_of(_mergeOnFields->begin(), _mergeOnFields->end(), [&](auto onField) {
                return dupKeyPattern.hasField(onField.fullPath());
            });

        if (_mergeProcessor->getMergeStrategyDescriptor().mode ==
                MergeStrategyDescriptor::kFailInsertMode &&
            dupKeyFromMatchingOnFields) {
            uassertStatusOKWithContext(
                ex.toStatus(),
                "$merge with whenMatched: fail found an existing document with "
                "the same values for the 'on' fields");

        } else {
            uassertStatusOKWithContext(ex.toStatus(), "$merge failed due to a DuplicateKey error");
        }
    } catch (const ExceptionFor<ErrorCodes::StaleEpoch>& ex) {
        // Convert a StaleEpoch into a QueryPlanKilled error to avoid automatically retrying the
        // operation from a router role loop.
        uasserted(ErrorCodes::QueryPlanKilled, ex.toStatus().reason());
    }
}

BatchedCommandRequest MergeStage::makeBatchedWriteRequest() const {
    return _mergeProcessor->getMergeStrategyDescriptor().batchedCommandGenerator(pExpCtx,
                                                                                 _outputNs);
}

void MergeStage::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceMergeBatch,
        pExpCtx->getOperationContext(),
        "hangWhileBuildingDocumentSourceMergeBatch",
        []() {
            LOGV2(
                20900,
                "Hanging aggregation due to 'hangWhileBuildingDocumentSourceMergeBatch' failpoint");
        },
        _outputNs);
}

}  // namespace exec::agg
}  // namespace mongo
