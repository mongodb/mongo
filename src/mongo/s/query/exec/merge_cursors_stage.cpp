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

#include "mongo/s/query/exec/merge_cursors_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"

namespace mongo::exec::agg {

boost::intrusive_ptr<MergeCursorsStage> documentSourceMergeCursorsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto mergeCursorsDocumentSource =
        boost::dynamic_pointer_cast<DocumentSourceMergeCursors>(documentSource);

    tassert(10561401, "expected 'DocumentSourceMergeCursors' type", mergeCursorsDocumentSource);

    return make_intrusive<MergeCursorsStage>(mergeCursorsDocumentSource->getExpCtx(),
                                             mergeCursorsDocumentSource->populateMerger());
}

REGISTER_AGG_STAGE_MAPPING(mergeCursors,
                           DocumentSourceMergeCursors::id,
                           documentSourceMergeCursorsToStageFn);

MergeCursorsStage::MergeCursorsStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::shared_ptr<BlockingResultsMerger>& blockingResultMerger)
    : Stage(DocumentSourceMergeCursors::kStageName, expCtx),
      _blockingResultsMerger(blockingResultMerger) {
    // Populate the shard ids from the 'RemoteCursor'.
    recordRemoteCursorShardIds(_blockingResultsMerger->asyncResultsMergerParams().getRemotes());
}

void MergeCursorsStage::recordRemoteCursorShardIds(const std::vector<RemoteCursor>& remoteCursors) {
    for (const auto& remoteCursor : remoteCursors) {
        tassert(5549103, "Encountered invalid shard ID", !remoteCursor.getShardId().empty());
        _shardsWithCursors.emplace(std::string{remoteCursor.getShardId()});
    }
}

bool MergeCursorsStage::usedDisk() const {
    return _stats.planSummaryStats.usedDisk;
}

const SpecificStats* MergeCursorsStage::getSpecificStats() const {
    return &_stats;
}

void MergeCursorsStage::detachFromOperationContext() {
    _blockingResultsMerger->detachFromOperationContext();
}

void MergeCursorsStage::reattachToOperationContext(OperationContext* opCtx) {
    _blockingResultsMerger->reattachToOperationContext(opCtx);
}

GetNextResult MergeCursorsStage::doGetNext() {
    auto next = uassertStatusOK(_blockingResultsMerger->next(pExpCtx->getOperationContext()));
    _stats.dataBearingNodeMetrics.add(_blockingResultsMerger->takeMetrics());
    if (next.isEOF()) {
        return GetNextResult::makeEOF();
    }
    Document doc = Document::fromBsonWithMetaData(*next.getResult());
    if (MONGO_unlikely(doc.metadata().isChangeStreamControlEvent())) {
        // Special handling needed for control events here to avoid an assertion failure in the
        // 'GetNextResult' constructor.
        return GetNextResult::makeAdvancedControlDocument(std::move(doc));
    }
    return GetNextResult(std::move(doc));
}

void MergeCursorsStage::doDispose() {
    _blockingResultsMerger->kill(getContext()->getOperationContext());
}

void MergeCursorsStage::doForceSpill() {
    auto status = _blockingResultsMerger->releaseMemory();
    uassertStatusOK(status);
}

boost::optional<query_stats::DataBearingNodeMetrics> MergeCursorsStage::takeRemoteMetrics() {
    auto metrics = _stats.dataBearingNodeMetrics;
    _stats.dataBearingNodeMetrics = {};
    return metrics;
}

std::size_t MergeCursorsStage::getNumRemotes() const {
    return _blockingResultsMerger->getNumRemotes();
}

BSONObj MergeCursorsStage::getHighWaterMark() {
    return _blockingResultsMerger->getHighWaterMark();
}

bool MergeCursorsStage::remotesExhausted() const {
    return _blockingResultsMerger->remotesExhausted();
}

Status MergeCursorsStage::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return _blockingResultsMerger->setAwaitDataTimeout(awaitDataTimeout);
}

void MergeCursorsStage::addNewShardCursors(std::vector<RemoteCursor>&& newCursors,
                                           const ShardTag& tag) {
    recordRemoteCursorShardIds(newCursors);
    _blockingResultsMerger->addNewShardCursors(std::move(newCursors), tag);
}

void MergeCursorsStage::closeShardCursors(const stdx::unordered_set<ShardId>& shardIds,
                                          const ShardTag& tag) {
    _blockingResultsMerger->closeShardCursors(shardIds, tag);
}

void MergeCursorsStage::setInitialHighWaterMark(const BSONObj& highWaterMark) {
    _blockingResultsMerger->setInitialHighWaterMark(highWaterMark);
}

void MergeCursorsStage::setNextHighWaterMarkDeterminingStrategy(
    NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminer) {
    _blockingResultsMerger->setNextHighWaterMarkDeterminingStrategy(
        std::move(nextHighWaterMarkDeterminer));
}

}  // namespace mongo::exec::agg
