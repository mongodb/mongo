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
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote_gen.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo {

/**
 * The $search stage expands to multiple internal stages when parsed, namely
 * $_internalSearchMongotRemote and $_internalSearchIdLookup. $setVariableFromSubPipeline may also
 * be added to handle $$SEARCH_META assignment.
 *
 * We only ever make a DocumentSourceSearch for a pipeline to store it in the view catalog.
 * Desugaring must be done every time the view is called.
 */
class DocumentSourceSearch final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$search"_sd;
    static constexpr StringData kProtocolStoredFieldsName = "storedSource"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Verify that sortSpec do not contain dots after '$searchSortValues', as we expect it to only
     * contain top-level fields (no nested objects).
     */
    static void validateSortSpec(boost::optional<BSONObj> sortSpec);

    /**
     * In a sharded environment this stage generates a DocumentSourceSetVariableFromSubPipeline to
     * run on the merging shard. This function contains the logic that allows that stage to move
     * past shards only stages.
     */
    static bool canMovePastDuringSplit(const DocumentSource& ds);

    DocumentSourceSearch() = default;
    DocumentSourceSearch(BSONObj query,
                         const boost::intrusive_ptr<ExpressionContext> expCtx,
                         boost::optional<InternalSearchMongotRemoteSpec> spec,
                         boost::optional<long long> limit,
                         bool requireSearchSequenceToken = false,
                         bool pipelineNeedsSearchMeta = true)
        : DocumentSource(kStageName, expCtx),
          _searchQuery(query.getOwned()),
          _spec(spec),
          _queryReferencesSearchMeta(pipelineNeedsSearchMeta),
          _limit(limit),
          _requiresSearchSequenceToken(requireSearchSequenceToken) {}

    const char* getSourceName() const;
    StageConstraints constraints(Pipeline::SplitState pipeState) const override;
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    auto isStoredSource() const {
        return _searchQuery.hasField(kReturnStoredSourceArg)
            ? _searchQuery[kReturnStoredSourceArg].Bool()
            : false;
    }

    std::list<boost::intrusive_ptr<DocumentSource>> desugar();

    BSONObj getSearchQuery() const {
        return _searchQuery.getOwned();
    }

    boost::optional<long long> getLimit() const {
        return _limit;
    }

    bool getSearchPaginationFlag() {
        return _requiresSearchSequenceToken;
    }

    boost::optional<int> getIntermediateResultsProtocolVersion() const {
        // If it turns out that this stage is not running on a sharded collection, we don't want
        // to send the protocol version to mongot. If the protocol version is sent, mongot will
        // generate unmerged metadata documents that we won't be set up to merge.
        if (!pExpCtx->needsMerge || !_spec) {
            return boost::none;
        }
        return _spec->getMetadataMergeProtocolVersion();
    }

    boost::optional<BSONObj> getSortSpec() const {
        return _spec ? _spec->getSortSpec() : boost::none;
    }

    size_t getRemoteCursorId() {
        return _remoteCursorId;
    }

    void setRemoteCursorVars(boost::optional<BSONObj> remoteCursorVars) {
        if (remoteCursorVars) {
            _remoteCursorVars = remoteCursorVars->getOwned();
        }
    }

    boost::optional<BSONObj> getRemoteCursorVars() const {
        return _remoteCursorVars;
    }

    void setCursor(executor::TaskExecutorCursor cursor) {
        _cursor.emplace(std::move(cursor));
    }

    boost::optional<executor::TaskExecutorCursor> getCursor() {
        return std::move(_cursor);
    }

    void setMetadataCursor(executor::TaskExecutorCursor cursor) {
        _metadataCursor.emplace(std::move(cursor));
    }

    boost::optional<executor::TaskExecutorCursor> getMetadataCursor() {
        return std::move(_metadataCursor);
    }

private:
    virtual Value serialize(
        const SerializationOptions& opts = SerializationOptions{}) const final override;

    GetNextResult doGetNext() {
        // We should never execute a DocumentSourceSearch.
        MONGO_UNREACHABLE_TASSERT(6253716);
    }

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) override;

    // The original stage specification that was the value for the "$search" field of the owning
    // object.
    BSONObj _searchQuery;

    /**
     * Valid in a sharded environment and holding information returned from planShardedSearch call,
     * including metadataMergeProtocolVersion, sortSpec, and mergingPipeline.
     *
     * TODO SERVER-87077 This _spec should not be optional. It should be constructed on construction
     * of the document source, which can enable us to remove fields like _queryReferencesSearchMeta,
     * _limit, and _requiresSearchSequenceToken that will just live in the _spec instead.
     */
    boost::optional<InternalSearchMongotRemoteSpec> _spec;

    /**
     * Flag indicating whether or not the total user pipeline references the $$SEARCH_META variable.
     * If true on mongos, we will insert a $setVariableFromSubPipeline stage into the merging
     * pipeline to provide it. If true on mongod, we will create a second plan executor and cursor
     * to handle the metadata pipeline (see generateMetadataPipelineAndAttachCursorsForSearch).
     */
    bool _queryReferencesSearchMeta = true;

    /**
     * This will populate the docsRequested field of the cursorOptions document sent as part of the
     * command to mongot in the case where the query has an extractable limit that can guide the
     * number of documents that mongot returns to mongod.
     */
    boost::optional<long long> _limit;

    /***
     * Flag indicating if the stage following the search query references $searchSequenceToken. This
     * will be passed to mongot_cursor
     *
     */
    bool _requiresSearchSequenceToken = false;

    // An unique id of search stage in the pipeline, currently it is hard coded to 0 because we can
    // only have one search stage and sub-pipelines are not in the same PlanExecutor.
    // We should assign unique ids when we have everything in a single PlanExecutorSBE.
    size_t _remoteCursorId{0};

    // The mongot data and metadata cursors for search. We establish the cursors before query
    // planning, use this object to temporarily store the cursors, and will transfer the cursors to
    // corresponding SBE executors when build PlanExecutorSBE.
    boost::optional<executor::TaskExecutorCursor> _cursor;
    boost::optional<executor::TaskExecutorCursor> _metadataCursor;
    boost::optional<BSONObj> _remoteCursorVars;
};

}  // namespace mongo
