/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_transform.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamTransform,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamTransform::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamTransform, DocumentSourceChangeStreamTransform::id)

boost::intrusive_ptr<DocumentSourceChangeStreamTransform>
DocumentSourceChangeStreamTransform::create(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            const DocumentSourceChangeStreamSpec& spec) {
    return new DocumentSourceChangeStreamTransform(expCtx, spec);
}

boost::intrusive_ptr<DocumentSourceChangeStreamTransform>
DocumentSourceChangeStreamTransform::createFromBson(
    BSONElement rawSpec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467601,
            "the '$_internalChangeStreamTransform' object spec must be an object",
            rawSpec.type() == BSONType::object);
    auto spec =
        DocumentSourceChangeStreamSpec::parse(rawSpec.Obj(), IDLParserContext("$changeStream"));

    // Set the change stream spec on the expression context.
    expCtx->setChangeStreamSpec(spec);

    auto canUseSupportedEvents = [&]() {
        if (expCtx->getInRouter()) {
            // 'supportedEvents' are not supposed to be used on a router.
            return false;
        }

        const auto* shardingState = ShardingState::get(expCtx->getOperationContext());
        if (!shardingState) {
            // Sharding state is not initialized. This is the case in unit tests and also on
            // standalone mongods. But on standalone mongods we do not support change streams, so we
            // will never get here.
            return true;
        }

        // Also 'supportedEvents' cannot be used in a non-shard replica set.
        auto role = shardingState->pollClusterRole();
        const bool isReplSet = !role.has_value();
        return !isReplSet;
    };
    uassert(10498501,
            "Expecting 'supportedEvents' to be set only on a shard mongod, not on router or "
            "replica set member",
            canUseSupportedEvents());

    return new DocumentSourceChangeStreamTransform(expCtx, std::move(spec));
}

DocumentSourceChangeStreamTransform::DocumentSourceChangeStreamTransform(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceChangeStreamSpec spec)
    : DocumentSourceInternalChangeStreamStage(DocumentSourceChangeStreamTransform::kStageName,
                                              expCtx),
      _changeStreamSpec(std::move(spec)),
      _transformer(std::make_shared<ChangeStreamEventTransformer>(expCtx, _changeStreamSpec)),
      _isIndependentOfAnyCollection(expCtx->getNamespaceString().isCollectionlessAggregateNS()) {

    // Extract the resume token or high-water-mark from the spec.
    auto tokenData = change_stream::resolveResumeTokenFromSpec(expCtx, _changeStreamSpec);

    // Set the initialPostBatchResumeToken on the expression context.
    expCtx->setInitialPostBatchResumeToken(ResumeToken(tokenData).toBSON());
}

StageConstraints DocumentSourceChangeStreamTransform::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);

    // This transformation could be part of a 'collectionless' change stream on an entire
    // database or cluster, mark as independent of any collection if so.
    constraints.isIndependentOfAnyCollection = _isIndependentOfAnyCollection;
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

Value DocumentSourceChangeStreamTransform::serialize(const SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(Document{{DocumentSourceChangeStream::kStageName,
                               Document{{"stage"_sd, "internalTransform"_sd},
                                        {"options"_sd, _changeStreamSpec.toBSON(opts)}}}});
    }

    // Internal change stream stages are not serialized for query stats. Query stats uses this stage
    // to serialize the user specified stage, and therefore if serializing for query stats, we
    // should use the '$changeStream' stage name.
    auto stageName = (opts.isSerializingForQueryStats())
        ? DocumentSourceChangeStream::kStageName
        : DocumentSourceChangeStreamTransform::kStageName;
    return Value(Document{{stageName, _changeStreamSpec.toBSON(opts)}});
}

DepsTracker::State DocumentSourceChangeStreamTransform::getDependencies(DepsTracker* deps) const {
    deps->fields.merge(_transformer->getFieldNameDependencies());
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamTransform::getModifiedPaths() const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

}  // namespace mongo
