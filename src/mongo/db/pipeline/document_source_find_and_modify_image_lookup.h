// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
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
#include <string_view>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_INTERNAL_DERIVED(FindAndModifyImageLookup);

/**
 * This stage will take a list of oplog entry documents as input and forge a no-op pre- or
 * post-image to be returned before the document for each 'findAndModify' oplog entry that has the
 * the 'needsRetryImage' field or each 'applyOps' oplog entry with a 'findAndModify' operation entry
 * that has the 'needsRetryImage' field. This stage also downconverts 'findAndModify' or 'applyOps'
 * oplog entry documents by stripping the 'needsRetryImage' field and appending the appropriate
 * 'preImageOpTime' or 'postImageOpTime' field. If 'includeCommitTransactionTimestamp' is true,
 * the forged pre- or post-image oplog entry document for each 'applyOps' oplog entry document that
 * comes with a transaction commit timestamp will have the commit timestamp attached to it.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceFindAndModifyImageLookup
    : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalFindAndModifyImageLookup"sv;
    static constexpr std::string_view kIncludeCommitTransactionTimestampFieldName =
        "includeCommitTransactionTimestamp"sv;

    static boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool includeCommitTransactionTimestamp = false);

    static boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup> createFromBson(
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
        return DocumentSourceFindAndModifyImageLookup::kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    bool getIncludeCommitTransactionTimestamp() const {
        return _includeCommitTransactionTimestamp;
    }

private:
    DocumentSourceFindAndModifyImageLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           bool includeCommitTransactionTimestamp);

    // Set to true if the input oplog entry documents can have a transaction commit timestamp
    // attached to it.
    bool _includeCommitTransactionTimestamp;
};
}  // namespace mongo
