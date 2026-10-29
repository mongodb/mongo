// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamCheckTopologyChange);
using ChangeStreamCheckTopologyChangeLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamCheckTopologyChangeStageParams>;

/**
 * TODO SERVER-112325: Remove this stage once no MongoDB version generates 'migrateChunkToNewShard'
 * events.
 *
 * This stage detected change stream topology changes in the form of 'kNewShardDetectedOpType'
 * events and forwarded them directly to the executor via an exception. Using an exception bypassed
 * the rest of the pipeline, ensuring that the event cannot be filtered out or modified by
 * user-specified stages and that it will ultimately be available to the mongoS.
 *
 * The mongoS needed to see all 'kNewShardDetectedOpType' events, so that it knows when it needs to
 * open cursors on newly active shards. These events were generated when a chunk is migrated to a
 * shard that previously may not have held any data for the collection being watched, and they
 * contained the information necessary for the mongoS to include the new shard in the merged change
 * stream.
 *
 * This stage is only there for backwards-compatibility reasons. It is necessary to keep this stage
 * for now because old versions of mongos can still create change stream pipelines creating this
 * stage. However, the stage is now a no-op, so it can be removed in the future.
 */
class DocumentSourceChangeStreamCheckTopologyChange final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamCheckTopologyChange"sv;

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckTopologyChange> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckTopologyChange> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceChangeStreamCheckTopologyChange(expCtx);
    }

    std::string_view getSourceName() const final {
        return kStageName;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    DocumentSourceChangeStreamCheckTopologyChange(
        const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {}
};

}  // namespace mongo
