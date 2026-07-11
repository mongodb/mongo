// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/merge_cursors_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"

#include <utility>

namespace mongo::exec::agg {

namespace {
// Ensure the ShardRegistry knows every shard we hold a cursor on before the merger opens its first
// getMore egress connection.
void validateShardIds(OperationContext* opCtx, const std::vector<RemoteCursor>& remotes) {
    auto* grid = opCtx ? Grid::get(opCtx) : nullptr;
    if (!grid || !grid->isInitialized()) {
        // No ShardRegistry wired up (e.g. a non-sharded context or some unit tests); nothing to do.
        return;
    }
    auto* shardRegistry = grid->shardRegistry();

    // Deduplicate shardIds: the registry cache is a single whole-registry entry, so the first
    // resolve warms every shard; the rest are cache hits.
    stdx::unordered_set<ShardId> seen;
    seen.reserve(remotes.size());
    for (const auto& remote : remotes) {
        ShardId shardId{std::string{remote.getShardId()}};
        if (!seen.insert(shardId).second) {
            continue;
        }
        // 'getShard()' also resolves the config shard (which 'getAllShardIds()' omits) and retries
        // via a forced reload on a miss, making this resilient to add/remove-shard churn.
        uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
    }
}
}  // namespace

boost::intrusive_ptr<MergeCursorsStage> documentSourceMergeCursorsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto mergeCursorsDocumentSource =
        boost::dynamic_pointer_cast<DocumentSourceMergeCursors>(documentSource);

    tassert(10561401, "expected 'DocumentSourceMergeCursors' type", mergeCursorsDocumentSource);

    auto blockingResultsMerger = mergeCursorsDocumentSource->populateMerger();

    validateShardIds(mergeCursorsDocumentSource->getExpCtx()->getOperationContext(),
                     blockingResultsMerger->asyncResultsMergerParams().getRemotes());

    return make_intrusive<MergeCursorsStage>(mergeCursorsDocumentSource->getExpCtx(),
                                             std::move(blockingResultsMerger));
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

void MergeCursorsStage::enableUndoNextMode() {
    _blockingResultsMerger->enableUndoNextMode();
}

void MergeCursorsStage::disableUndoNextMode() {
    _blockingResultsMerger->disableUndoNextMode();
}

void MergeCursorsStage::undoNext() {
    _blockingResultsMerger->undoNext();
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

void MergeCursorsStage::recognizeControlEvents() {
    _blockingResultsMerger->recognizeControlEvents();
}

void MergeCursorsStage::setHighWaterMark(const BSONObj& highWaterMark) {
    _blockingResultsMerger->setHighWaterMark(highWaterMark);
}

void MergeCursorsStage::setNextHighWaterMarkDeterminingStrategy(
    NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminer) {
    _blockingResultsMerger->setNextHighWaterMarkDeterminingStrategy(
        std::move(nextHighWaterMarkDeterminer));
}

}  // namespace mongo::exec::agg
