/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"

#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/resharding/common_types_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalReshardingOwnershipMatch,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceReshardingOwnershipMatch::createFromBson,
                                  true);

boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch>
DocumentSourceReshardingOwnershipMatch::create(
    ShardId recipientShardId,
    ShardKeyPattern reshardingKey,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceReshardingOwnershipMatch(
        std::move(recipientShardId), std::move(reshardingKey), expCtx);
}

boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch>
DocumentSourceReshardingOwnershipMatch::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(8423307,
            str::stream() << "Argument to " << kStageName << " must be an object",
            elem.type() == Object);

    auto parsed = DocumentSourceReshardingOwnershipMatchSpec::parse(
        {"DocumentSourceReshardingOwnershipMatchSpec"}, elem.embeddedObject());

    return new DocumentSourceReshardingOwnershipMatch(
        parsed.getRecipientShardId(), ShardKeyPattern(parsed.getReshardingKey()), expCtx);
}

DocumentSourceReshardingOwnershipMatch::DocumentSourceReshardingOwnershipMatch(
    ShardId recipientShardId,
    ShardKeyPattern reshardingKey,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      _recipientShardId{std::move(recipientShardId)},
      _reshardingKey{std::move(reshardingKey)} {}

StageConstraints DocumentSourceReshardingOwnershipMatch::constraints(
    Pipeline::SplitState pipeState) const {
    return StageConstraints(StreamType::kStreaming,
                            PositionRequirement::kNone,
                            HostTypeRequirement::kAnyShard,
                            DiskUseRequirement::kNoDiskUse,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed,
                            ChangeStreamRequirement::kDenylist);
}

Value DocumentSourceReshardingOwnershipMatch::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value{Document{{kStageName,
                           DocumentSourceReshardingOwnershipMatchSpec(
                               _recipientShardId, _reshardingKey.getKeyPattern())
                               .toBSON()}}};
}

DepsTracker::State DocumentSourceReshardingOwnershipMatch::getDependencies(
    DepsTracker* deps) const {
    for (const auto& skElem : _reshardingKey.toBSON()) {
        deps->fields.insert(skElem.fieldNameStringData().toString());
    }

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceReshardingOwnershipMatch::getModifiedPaths() const {
    // This stage does not modify or rename any paths.
    return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
}

DocumentSource::GetNextResult DocumentSourceReshardingOwnershipMatch::doGetNext() {
    if (!_tempReshardingChunkMgr) {
        // TODO: Actually propagate the temporary resharding namespace from the recipient.
        auto tempReshardingNss =
            resharding::constructTemporaryReshardingNss(pExpCtx->ns.db(), *pExpCtx->uuid);

        auto* catalogCache = Grid::get(pExpCtx->opCtx)->catalogCache();
        _tempReshardingChunkMgr =
            uassertStatusOK(catalogCache->getShardedCollectionRoutingInfoWithRefresh(
                pExpCtx->opCtx, tempReshardingNss));
    }

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        auto shardKey =
            _reshardingKey.extractShardKeyFromDocThrows(nextInput.getDocument().toBson());

        if (_tempReshardingChunkMgr->keyBelongsToShard(shardKey, _recipientShardId)) {
            return nextInput;
        }

        // For performance reasons, a streaming stage must not keep references to documents across
        // calls to getNext(). Such stages must retrieve a result from their child and then release
        // it (or return it) before asking for another result. Failing to do so can result in extra
        // work, since the Document/Value library must copy data on write when that data has a
        // refcount above one.
        nextInput.releaseDocument();
    }

    return nextInput;
}

}  // namespace mongo
