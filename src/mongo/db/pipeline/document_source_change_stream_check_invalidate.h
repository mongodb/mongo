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
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamCheckInvalidate);
using ChangeStreamCheckInvalidateLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamCheckInvalidateStageParams>;

/**
 * This stage is used internally for change stream notifications to artificially generate an
 * "invalidate" entry for commands that should invalidate the change stream (e.g. collection drop
 * for a single-collection change stream). It is not intended to be created by the user.
 */
class DocumentSourceChangeStreamCheckInvalidate final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamCheckInvalidate"sv;

    std::string_view getSourceName() const final {
        // This is used in error reporting.
        return DocumentSourceChangeStreamCheckInvalidate::kStageName;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
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

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert(std::string{DocumentSourceChangeStream::kOperationTypeField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kClusterTimeField});
        deps->setNeedsMetadata(DocumentMetadataFields::MetaType::kSortKey);
        return DepsTracker::State::SEE_NEXT;
    }

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckInvalidate> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckInvalidate> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Whether the current aggregation command in the 'ExpressionContext' can lead to change stream
     * invalidate events being issued. Currently, invalidate events can only occur in
     * collection-level and database-level change streams, but not in all-cluster change streams.
     */
    static bool canInvalidateEventOccur(const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamCheckInvalidateToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Use the create static method to create a DocumentSourceChangeStreamCheckInvalidate.
     */
    DocumentSourceChangeStreamCheckInvalidate(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              boost::optional<ResumeTokenData> startAfterInvalidate)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx),
          _startAfterInvalidate(std::move(startAfterInvalidate)) {
        tassert(11294807,
                "Expected the passed resume token to be from an invalidate notification",
                !_startAfterInvalidate ||
                    _startAfterInvalidate->fromInvalidate == ResumeTokenData::kFromInvalidate);
    }

    boost::optional<ResumeTokenData> _startAfterInvalidate;
};

}  // namespace mongo
