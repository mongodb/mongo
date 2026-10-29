// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_ownership_match_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>
#include <utility>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceReshardingOwnershipMatchToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* reshardingOwnershipMatchDS =
        dynamic_cast<DocumentSourceReshardingOwnershipMatch*>(documentSource.get());

    tassert(10812500,
            "expected 'DocumentSourceReshardingOwnershipMatch' type",
            reshardingOwnershipMatchDS);

    return make_intrusive<exec::agg::ReshardingOwnershipMatchStage>(
        reshardingOwnershipMatchDS->kStageName,
        reshardingOwnershipMatchDS->getExpCtx(),
        reshardingOwnershipMatchDS->_recipientShardId,
        reshardingOwnershipMatchDS->_reshardingKey,
        reshardingOwnershipMatchDS->_temporaryReshardingNamespace);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(reshardingOwnershipMatchStage,
                           DocumentSourceReshardingOwnershipMatch::id,
                           documentSourceReshardingOwnershipMatchToStageFn);

ReshardingOwnershipMatchStage::ReshardingOwnershipMatchStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    ShardId recipientShardId,
    const std::shared_ptr<ShardKeyPattern>& reshardingKey,
    boost::optional<NamespaceString> temporaryReshardingNamespace)
    : Stage(stageName, pExpCtx),
      _recipientShardId(std::move(recipientShardId)),
      _reshardingKey(reshardingKey),
      _temporaryReshardingNamespace(std::move(temporaryReshardingNamespace)) {};

GetNextResult ReshardingOwnershipMatchStage::doGetNext() {
    if (!_tempReshardingChunkMgr) {
        auto* catalogCache = Grid::get(pExpCtx->getOperationContext())->catalogCache();
        auto tempNss = _temporaryReshardingNamespace
            ? _temporaryReshardingNamespace.value()
            : resharding::constructTemporaryReshardingNss(pExpCtx->getNamespaceString(),
                                                          *pExpCtx->getUUID());
        _tempReshardingChunkMgr =
            uassertStatusOK(catalogCache->getCollectionPlacementInfoWithRefresh(
                pExpCtx->getOperationContext(), tempNss));
    }

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        auto shardKey =
            _reshardingKey->extractShardKeyFromDocThrows(nextInput.getDocument().toBson());

        if (_tempReshardingChunkMgr->keyBelongsToShard(shardKey, _recipientShardId)) {
            return nextInput;
        }

        // For performance reasons, a streaming stage must not keep references to documents
        // across calls to getNext(). Such stages must retrieve a result from their child and
        // then release it (or return it) before asking for another result. Failing to do so can
        // result in extra work, since the Document/Value library must copy data on write when
        // that data has a refcount above one.
        nextInput.releaseDocument();
    }

    return nextInput;
}
}  // namespace exec::agg
}  // namespace mongo
