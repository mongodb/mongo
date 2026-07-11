// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string>
#include <string_view>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class is not a registered stage, it is only used as an optimized replacement for $sample
 * when the storage engine allows us to use a random cursor.
 */
class DocumentSourceSampleFromRandomCursor final : public DocumentSource {
public:
    static constexpr std::string_view kStageName{"$sampleFromRandomCursor"};
    std::string_view getSourceName() const final;
    Value serialize(const query_shape::SerializationOptions& opts = {}) const final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kFirst,
                HostTypeRequirement::kTargetedShards,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    static boost::intrusive_ptr<DocumentSourceSampleFromRandomCursor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        long long size,
        std::string idField,
        long long collectionSize);

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage> sampleFromRandomCursorStageToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSourceSampleFromRandomCursor);

    DocumentSourceSampleFromRandomCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         long long size,
                                         std::string idField,
                                         long long collectionSize);

    long long _size;

    // The field to use as the id of a document. Usually '_id', but 'ts' for the oplog.
    std::string _idField;

    // The approximate number of documents in the collection (includes orphans).
    const long long _nDocsInColl;
};

}  // namespace mongo
