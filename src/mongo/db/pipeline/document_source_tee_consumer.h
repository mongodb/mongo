// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <set>
#include <string>
#include <string_view>

#include <boost/intrusive_ptr.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class Document;
class ExpressionContext;
class Value;

/**
 * This stage acts as a proxy between a pipeline within a $facet stage and the buffer of incoming
 * documents held in a TeeBuffer stage. It will simply open an iterator on the TeeBuffer stage, and
 * answer calls to getNext() by advancing said iterator.
 */
class DocumentSourceTeeConsumer : public DocumentSource {
public:
    static boost::intrusive_ptr<DocumentSourceTeeConsumer> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        size_t facetId,
        std::string_view stageName);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};
        constraints.outputDependsOnSingleInput = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    /**
     * Returns SEE_NEXT, since it requires no fields, and changes nothing about the documents.
     */
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    std::string_view getSourceName() const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceTeeConsumerToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    DocumentSourceTeeConsumer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              size_t facetId,
                              std::string_view stageName);

    size_t _facetId;

    // Specific name of the tee consumer.
    std::string _stageName;
};
}  // namespace mongo
