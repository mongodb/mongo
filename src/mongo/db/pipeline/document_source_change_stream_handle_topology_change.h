// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamHandleTopologyChange);
using ChangeStreamHandleTopologyChangeLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamHandleTopologyChangeStageParams>;

/**
 * An internal stage used as part of the change streams infrastructure to listen for an event
 * signaling that a new shard now has potentially matching data. For example, this stage will
 * detect if a collection is being watched and a chunk for that collection migrates to a shard for
 * the first time. When this event is detected, this stage will establish a new cursor on that
 * shard and add it to the cursors being merged.
 */
class DocumentSourceChangeStreamHandleTopologyChange final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr std::string_view kStageName =
        change_stream_constants::stage_names::kHandleTopologyChange;

    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    /**
     * Creates a new stage which will establish a new cursor and add it to the cursors being merged
     * by 'mergeCursorsStage' whenever a new shard is detected by a change stream.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    std::string_view getSourceName() const final {
        return kStageName;
    }

    Value doSerialize(const query_shape::SerializationOptions& opts =
                          query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // This stage neither modifies nor renames any field.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return DistributedPlanLogic{nullptr, this, change_stream_constants::kSortSpec};
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert(std::string{DocumentSourceChangeStream::kOperationTypeField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kNamespaceField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kFullDocumentField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kClusterTimeField});
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    DocumentSourceChangeStreamHandleTopologyChange(const boost::intrusive_ptr<ExpressionContext>&);
};
}  // namespace mongo
