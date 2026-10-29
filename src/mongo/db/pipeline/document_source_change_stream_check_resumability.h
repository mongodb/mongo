// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamCheckResumability);
using ChangeStreamCheckResumabilityLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamCheckResumabilityStageParams>;

/**
 * This stage checks whether or not the oplog has enough history to resume the stream, and consumes
 * all events up to the given resume point. It is deployed on all shards when resuming a stream on
 * a sharded cluster, and is also used in the single-replicaset case when a stream is opened with
 * startAtOperationTime or with a high-water-mark resume token. It defers to the COLLSCAN to check
 * whether the first event (matching or non-matching) encountered in the oplog has a timestamp equal
 * to or earlier than the minTs in the change stream filter. If not, the COLLSCAN will throw an
 * assertion, which this stage catches and converts into a more comprehensible $changeStream
 * specific exception. The rules are:
 *
 * - If the first event seen in the oplog has the same timestamp as the requested resume token or
 *   startAtOperationTime, we can resume.
 * - If the timestamp of the first event seen in the oplog is earlier than the requested resume
 *   token or startAtOperationTime, we can resume.
 * - If the first entry in the oplog is a replica set initialization, then we can resume even if the
 *   token timestamp is earlier, since no events can have fallen off this oplog yet. This can happen
 *   in a sharded cluster when a new shard is added.
 *
 * - Otherwise we cannot resume, as we do not know if there were any events between the resume token
 *   and the first matching document in the oplog.
 */
class DocumentSourceChangeStreamCheckResumability : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamCheckResumability"sv;

    std::string_view getSourceName() const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kTargetedShards,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);
        constraints.consumesLogicalCollectionData = false;
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        deps->setNeedsMetadata(DocumentMetadataFields::MetaType::kSortKey);
        return DepsTracker::State::SEE_NEXT;
    }

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const override;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckResumability> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckResumability> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static const Id& id;

    Id getId() const override {
        return id;
    }

protected:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamCheckResumabilityToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Use the create static method to create a DocumentSourceChangeStreamCheckResumability.
     */
    DocumentSourceChangeStreamCheckResumability(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token);

    const ResumeTokenData _tokenFromClient;
};
}  // namespace mongo
