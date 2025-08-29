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

#include "mongo/db/s/resharding/resharding_ownership_match_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

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
    StringData stageName,
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
