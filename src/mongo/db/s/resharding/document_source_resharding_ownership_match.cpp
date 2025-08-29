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


#include "mongo/db/s/resharding/document_source_resharding_ownership_match.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalReshardingOwnershipMatch,
                                  LiteParsedDocumentSourceInternal::parse,
                                  DocumentSourceReshardingOwnershipMatch::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalReshardingOwnershipMatch,
                            DocumentSourceReshardingOwnershipMatch::id)

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
            elem.type() == BSONType::object);

    auto parsed = DocumentSourceReshardingOwnershipMatchSpec::parse(
        elem.embeddedObject(), IDLParserContext{"DocumentSourceReshardingOwnershipMatchSpec"});

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
      _reshardingKey{std::make_shared<ShardKeyPattern>(std::move(reshardingKey))},
      _temporaryReshardingNamespace{std::move(temporaryReshardingNamespace)} {}

StageConstraints DocumentSourceReshardingOwnershipMatch::constraints(
    PipelineSplitState pipeState) const {
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
                                                           _reshardingKey->getKeyPattern());
    // TODO SERVER-92437 ensure this behavior is safe during FCV upgrade/downgrade
    if (resharding::gFeatureFlagReshardingRelaxedMode.isEnabled(
            VersionContext::getDecoration(getExpCtx()->getOperationContext()),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        spec.setTemporaryReshardingNamespace(_temporaryReshardingNamespace);
    }
    return Value{Document{{kStageName, spec.toBSON(opts)}}};
}

DepsTracker::State DocumentSourceReshardingOwnershipMatch::getDependencies(
    DepsTracker* deps) const {
    for (const auto& skElem : _reshardingKey->toBSON()) {
        deps->fields.insert(std::string{skElem.fieldNameStringData()});
    }

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceReshardingOwnershipMatch::getModifiedPaths() const {
    // This stage does not modify or rename any paths.
    return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
}

}  // namespace mongo
