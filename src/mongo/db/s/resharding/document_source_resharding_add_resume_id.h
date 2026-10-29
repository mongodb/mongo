// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
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

DEFINE_LITE_PARSED_STAGE_INTERNAL_DERIVED(ReshardingAddResumeId);

/**
 * This stage is responsible for attaching the resharding's _id field to all the input oplog entry
 * documents. For a document that corresponds to an applyOps oplog entry for a committed
 * transaction, this will be {clusterTime: <transaction commit timestamp>, ts: <applyOps
 * optime.ts>}. For all other documents, this will be {clusterTime: <optime.ts>, ts: <optime.ts>}.
 */
class [[MONGO_MOD_PUBLIC]] DocumentSourceReshardingAddResumeId : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_addReshardingResumeId"sv;

    static boost::intrusive_ptr<DocumentSourceReshardingAddResumeId> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSourceReshardingAddResumeId> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const override {
        return DocumentSourceReshardingAddResumeId::kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    DocumentSourceReshardingAddResumeId(const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
