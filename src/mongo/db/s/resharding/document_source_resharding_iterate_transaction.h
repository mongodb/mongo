// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
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

DEFINE_LITE_PARSED_STAGE_INTERNAL_DERIVED(ReshardingIterateTransaction);

/**
 * This stage keeps track of applyOps oplog entries that represent transactions and iterates them
 * whenever an oplog entry commits a transaction. When the stage observes an applyOps or commit
 * command that commits a transaction, it emits one document for each applyOps in the transaction.
 *
 * If 'includeCommitTransactionTimestamp' is true, this stage is responsible for attaching the
 * transaction commit timestamp to each applyOps oplog entry document that it emits and a
 * downstream stage is expected to use this timestamp when generating the resharding's _id field
 * for the document (as described below).
 *
 * If 'includeCommitTransactionTimestamp' is false, this stage is responsible for generating
 * the resharding's _id field for each oplog entry document that it emits. For a document that
 * corresponds to an applyOps oplog entry for a committed transaction, this will be
 * {clusterTime: <transaction commit timestamp>, ts: <applyOps optime.ts>}. For all other documents,
 * this will be {clusterTime: <optime.ts>, ts: <optime.ts>}.
 */
class [[MONGO_MOD_PUBLIC]] DocumentSourceReshardingIterateTransaction : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalReshardingIterateTransaction"sv;
    static constexpr std::string_view kIncludeCommitTransactionTimestampFieldName =
        "includeCommitTransactionTimestamp"sv;

    static boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool includeCommitTransactionTimestamp = false);

    static boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    std::string_view getSourceName() const override {
        return DocumentSourceReshardingIterateTransaction::kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    bool getIncludeCommitTransactionTimestamp() const {
        return _includeCommitTransactionTimestamp;
    }

private:
    DocumentSourceReshardingIterateTransaction(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool includeCommitTransactionTimestamp);

    // Set to true if this stage should attach the transaction commit timestamp to the applyOps
    // oplog entry documents that it emits instead of generating a resharding id for the documents
    // that it emits.
    bool _includeCommitTransactionTimestamp;
};

}  // namespace mongo
