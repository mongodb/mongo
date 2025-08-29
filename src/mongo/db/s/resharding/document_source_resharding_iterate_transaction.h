/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
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

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

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
class DocumentSourceReshardingIterateTransaction : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalReshardingIterateTransaction"_sd;
    static constexpr StringData kIncludeCommitTransactionTimestampFieldName =
        "includeCommitTransactionTimestamp"_sd;

    static boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        bool includeCommitTransactionTimestamp = false);

    static boost::intrusive_ptr<DocumentSourceReshardingIterateTransaction> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return DocumentSourceReshardingIterateTransaction::kStageName.data();
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
