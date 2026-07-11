// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_transform.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
// The following fields will be removed for query shape serialization.
const StringDataSet kFieldsToRemoveForQueryShapeSerialization = {"version", "supportedEvents"};
}  // namespace

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamTransform,
                                              ChangeStreamTransformLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamTransform,
                                                   DocumentSourceChangeStreamTransform,
                                                   ChangeStreamTransformStageParams);

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

    uassert(10498501,
            "Expecting 'supportedEvents' to be set only on a shard mongod, not on a router or "
            "non-shard replica set member",
            !spec.getSupportedEvents() || !change_stream::isRouterOrNonShardedReplicaSet(expCtx));

    // Set the change stream spec on the expression context.
    expCtx->setChangeStreamSpec(spec);

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

    constraints.preservesCardinality = true;
    // This transformation could be part of a 'collectionless' change stream on an entire
    // database or cluster, mark as independent of any collection if so.
    constraints.isIndependentOfAnyCollection = _isIndependentOfAnyCollection;
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;
    return constraints;
}

Value DocumentSourceChangeStreamTransform::serialize(
    const query_shape::SerializationOptions& opts) const {
    BSONObj serializedOptions = [&]() -> BSONObj {
        BSONObj serialized = _changeStreamSpec.toBSON(opts);

        if (opts.literalPolicy != query_shape::LiteralSerializationPolicy::kUnchanged) {
            // Explicitly remove specific fields from the '$changeStream' stage serialization for
            // query shapes that should not have any influence on the query shape hash computation.
            serialized = serialized.removeFields(kFieldsToRemoveForQueryShapeSerialization);
        }

        return serialized;
    }();

    if (opts.isSerializingForExplain()) {
        return Value(Document{
            {DocumentSourceChangeStream::kStageName,
             Document{{"stage"sv, "internalTransform"sv}, {"options"sv, serializedOptions}}}});
    }

    // Internal change stream stages are not serialized for query stats. Query stats uses this stage
    // to serialize the user specified stage, and therefore if serializing for query stats, we
    // should use the '$changeStream' stage name.
    auto stageName = (opts.isSerializingForQueryStats())
        ? DocumentSourceChangeStream::kStageName
        : DocumentSourceChangeStreamTransform::kStageName;
    return Value(Document{{stageName, serializedOptions}});
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
