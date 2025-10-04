/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

void MergeStage::flush(BatchedCommandRequest bcr, BatchedObjects batch) {
    try {
        DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
        _mergeProcessor->flush(_outputNs, std::move(bcr), std::move(batch));
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
