// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(ChangeStreamSplitLargeEvent);

class DocumentSourceChangeStreamSplitLargeEvent final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$changeStreamSplitLargeEvent"sv;

    static boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    // This stage does not reference any user or system variables.
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    void validatePipelinePosition(bool alreadyOptimized,
                                  DocumentSourceContainer::const_iterator pos,
                                  const DocumentSourceContainer& container) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setNeedsMetadata(DocumentMetadataFields::MetaType::kSortKey);
        return DepsTracker::State::SEE_NEXT;
    }

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamSplitLargeEventToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // This constructor is private, callers should use the 'create()' method above.
    DocumentSourceChangeStreamSplitLargeEvent(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              boost::optional<ResumeTokenData> resumeAfterSplit);

    boost::optional<ResumeTokenData> _resumeAfterSplit;
};

}  // namespace mongo
