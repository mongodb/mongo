// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamConvertErrors);
using ChangeStreamConvertErrorsLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamConvertErrorsStageParams>;

/**
 * The topmost stage of an opted-in ('showErrorsAsEvents: true') change stream pipeline. It converts
 * recoverable failures raised by the stages below it into 'operationType: "error"' events, built
 * from the identity recorded on the ExpressionContext; materializes a resume gap stashed below the
 * user stages; and releases an error event thrown past the router user stages.
 *
 * It is injected post-optimization and above every user stage, so the optimizer never sees it and
 * no user stage ever observes an error event. It is a DocumentSource rather than an exec-only
 * stage so that it is visible in explain.
 */
class DocumentSourceChangeStreamConvertErrors : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamConvertErrors"sv;

    std::string_view getSourceName() const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override;

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

    static boost::intrusive_ptr<DocumentSourceChangeStreamConvertErrors> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamConvertErrors> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Use the create static method to create a DocumentSourceChangeStreamConvertErrors.
     */
    DocumentSourceChangeStreamConvertErrors(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {}
};
}  // namespace mongo
