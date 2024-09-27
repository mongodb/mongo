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


#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

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
    boost::optional<NamespaceString> temporaryReshardingNamespace,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceReshardingOwnershipMatch(std::move(recipientShardId),
                                                      std::move(reshardingKey),
                                                      std::move(temporaryReshardingNamespace),
                                                      expCtx);
}

boost::intrusive_ptr<DocumentSourceReshardingOwnershipMatch>
DocumentSourceReshardingOwnershipMatch::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(8423307,
            str::stream() << "Argument to " << kStageName << " must be an object",
            elem.type() == Object);

    auto parsed = DocumentSourceReshardingOwnershipMatchSpec::parse(
        IDLParserContext{"DocumentSourceReshardingOwnershipMatchSpec"}, elem.embeddedObject());

    return new DocumentSourceReshardingOwnershipMatch(parsed.getRecipientShardId(),
                                                      ShardKeyPattern(parsed.getReshardingKey()),
                                                      parsed.getTemporaryReshardingNamespace(),
                                                      expCtx);
}

DocumentSourceReshardingOwnershipMatch::DocumentSourceReshardingOwnershipMatch(
    ShardId recipientShardId,
    ShardKeyPattern reshardingKey,
    boost::optional<NamespaceString> temporaryReshardingNamespace,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      _recipientShardId{std::move(recipientShardId)},
      _reshardingKey{std::move(reshardingKey)},
      _temporaryReshardingNamespace{std::move(temporaryReshardingNamespace)} {}

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

Value DocumentSourceReshardingOwnershipMatch::serialize(const SerializationOptions& opts) const {
    auto spec = DocumentSourceReshardingOwnershipMatchSpec(_recipientShardId,
                                                           _reshardingKey.getKeyPattern());
    // TODO SERVER-92437 ensure this behavior is safe during FCV upgrade/downgrade
    if (resharding::gFeatureFlagReshardingRelaxedMode.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        spec.setTemporaryReshardingNamespace(_temporaryReshardingNamespace);
    }
    return Value{Document{{kStageName, spec.toBSON(opts)}}};
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
        auto* catalogCache = Grid::get(pExpCtx->opCtx)->catalogCache();
        auto tempNss = _temporaryReshardingNamespace
            ? _temporaryReshardingNamespace.value()
            : resharding::constructTemporaryReshardingNss(pExpCtx->ns, *pExpCtx->uuid);
        _tempReshardingChunkMgr =
            uassertStatusOK(catalogCache->getTrackedCollectionRoutingInfoWithPlacementRefresh(
                                pExpCtx->opCtx, tempNss))
                .cm;
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
