// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_event_transform.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamTransform);
using ChangeStreamTransformLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamTransformStageParams>;

class DocumentSourceChangeStreamTransform final : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamTransform"sv;

    /**
     * Creates a new transformation stage from the given specification.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static boost::intrusive_ptr<DocumentSourceChangeStreamTransform> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Document applyTransformation(const Document& input);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    /**
     * This function should never be called, since this DocumentSource has its own serialize method.
     */
    Value doSerialize(const query_shape::SerializationOptions& opts) const final {
        MONGO_UNREACHABLE;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    const ChangeStreamEventTransformation::SupportedEvents& getSupportedEvents_forTest() const {
        return _transformer->getSupportedEvents_forTest();
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamTransformToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // This constructor is private, callers should use the 'create()' method above.
    DocumentSourceChangeStreamTransform(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        DocumentSourceChangeStreamSpec spec);

    DocumentSourceChangeStreamSpec _changeStreamSpec;

    // TODO SERVER-105521: Check if we can change from 'std::shared_ptr' to 'std::unique_ptr', and
    // std::move the transformer to the Stage.
    std::shared_ptr<ChangeStreamEventTransformer> _transformer;

    // Set to true if this transformation stage can be run on the collectionless namespace.
    bool _isIndependentOfAnyCollection;
};

}  // namespace mongo
