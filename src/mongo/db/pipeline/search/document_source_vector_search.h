/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo {

/**
 * A class to retrieve vector search results from a mongot process.
 */
class DocumentSourceVectorSearch : public DocumentSource {
public:
    const BSONObj kSortSpec = BSON("$vectorSearchScore" << -1);
    static constexpr StringData kStageName = "$vectorSearch"_sd;
    static constexpr StringData kLimitFieldName = "limit"_sd;
    static constexpr StringData kFilterFieldName = "filter"_sd;
    static constexpr StringData kIndexFieldName = "index"_sd;
    static constexpr StringData kNumCandidatesFieldName = "numCandidates"_sd;

    DocumentSourceVectorSearch(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               std::shared_ptr<executor::TaskExecutor> taskExecutor,
                               BSONObj originalSpec);

    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        DistributedPlanLogic logic;
        logic.shardsStage = this;
        if (_limit) {
            logic.mergingStages = {DocumentSourceLimit::create(pExpCtx, *_limit)};
        }
        logic.mergeSortPattern = kSortSpec;
        return logic;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : pExpCtx;
        return make_intrusive<DocumentSourceVectorSearch>(
            expCtx, _taskExecutor, _originalSpec.copy());
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.requiresInputDocSource = false;
        constraints.noFieldModifications = true;
        return constraints;
    };

protected:
    Value serialize(const SerializationOptions& opts) const override;

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) override;

private:
    // Get the next record from mongot. This will establish the mongot cursor on the first call.
    GetNextResult doGetNext() final;

    boost::optional<BSONObj> getNext();

    DocumentSource::GetNextResult getNextAfterSetup();

    // Initialize metrics related to the $vectorSearch stage on the OpDebug object.
    void initializeOpDebugVectorSearchMetrics();

    std::unique_ptr<MatchExpression> _filterExpr;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    std::unique_ptr<executor::TaskExecutorCursor> _cursor;

    // Store the cursorId. We need to store it on the document source because the id on the
    // TaskExecutorCursor will be set to zero after the final getMore after the cursor is
    // exhausted.
    boost::optional<CursorId> _cursorId{boost::none};

    // Limit value for the pipeline as a whole. This is not the limit that we send to mongot,
    // rather, it is used when adding the $limit stage to the merging pipeline in a sharded cluster.
    // This allows us to limit the documents that are returned from the shards as much as possible
    // without adding complicated rules for pipeline splitting.
    // The limit that we send to mongot is received and stored on the '_request' object above.
    boost::optional<long long> _limit;

    // Keep track of the original request BSONObj's extra fields in case there were fields mongod
    // doesn't know about that mongot will need later.
    BSONObj _originalSpec;
};
}  // namespace mongo
