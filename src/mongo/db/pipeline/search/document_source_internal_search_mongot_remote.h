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

#include <queue>

#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote_gen.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

/**
 * A class to retrieve $search results from a mongot process.
 */
class DocumentSourceInternalSearchMongotRemote : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalSearchMongotRemote"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static StageConstraints getSearchDefaultConstraints() {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.requiresInputDocSource = false;
        return constraints;
    }

    DocumentSourceInternalSearchMongotRemote(
        InternalSearchMongotRemoteSpec spec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::shared_ptr<executor::TaskExecutor> taskExecutor,
        boost::optional<long long> mongotDocsRequested = boost::none,
        bool requiresSearchSequenceToken = false)
        : DocumentSource(kStageName, expCtx),
          _mergingPipeline(spec.getMergingPipeline()
                               ? mongo::Pipeline::parse(*spec.getMergingPipeline(), expCtx)
                               : nullptr),
          _searchQuery(spec.getMongotQuery().getOwned()),
          _taskExecutor(taskExecutor),
          _metadataMergeProtocolVersion(spec.getMetadataMergeProtocolVersion()),
          _limit(spec.getLimit().value_or(0)),
          _queryReferencesSearchMeta(spec.getRequiresSearchMetaCursor().value_or(true)),
          _mongotDocsRequested(mongotDocsRequested),
          _requiresSearchSequenceToken(requiresSearchSequenceToken) {
        if (spec.getSortSpec().has_value()) {
            _sortSpec = spec.getSortSpec()->getOwned();
            _sortKeyGen.emplace(SortPattern{*_sortSpec, pExpCtx}, pExpCtx->getCollator());
        }
    }

    /**
     * Shorthand constructor from a mongot query only (e.g. no merging pipeline, limit, etc).
     */
    DocumentSourceInternalSearchMongotRemote(
        BSONObj searchQuery,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::shared_ptr<executor::TaskExecutor> taskExecutor,
        boost::optional<long long> mongotDocsRequested = boost::none,
        bool requiresSearchSequenceToken = false)
        : DocumentSource(kStageName, expCtx),
          _mergingPipeline(nullptr),
          _searchQuery(searchQuery.getOwned()),
          _taskExecutor(taskExecutor),
          _mongotDocsRequested(mongotDocsRequested),
          _requiresSearchSequenceToken(requiresSearchSequenceToken) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return getSearchDefaultConstraints();
    }

    const char* getSourceName() const override;

    virtual boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        // The desugaring of DocumentSourceSearch happens after sharded planning, so we should never
        // execute distributedPlanLogic here, instead it should be called against
        // DocumentSourceSearch.
        MONGO_UNREACHABLE_TASSERT(7815902);
    }

    virtual boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : pExpCtx;
        if (_metadataMergeProtocolVersion) {
            InternalSearchMongotRemoteSpec remoteSpec{_searchQuery, *_metadataMergeProtocolVersion};
            remoteSpec.setMergingPipeline(_mergingPipeline
                                              ? boost::optional<std::vector<mongo::BSONObj>>(
                                                    _mergingPipeline->serializeToBson())
                                              : boost::none);
            if (_sortSpec.has_value()) {
                remoteSpec.setSortSpec(_sortSpec->getOwned());
            }
            remoteSpec.setRequiresSearchMetaCursor(_queryReferencesSearchMeta);
            return make_intrusive<DocumentSourceInternalSearchMongotRemote>(
                std::move(remoteSpec), expCtx, _taskExecutor, _mongotDocsRequested);
        } else {
            return make_intrusive<DocumentSourceInternalSearchMongotRemote>(
                _searchQuery, expCtx, _taskExecutor, _mongotDocsRequested);
        }
    }

    BSONObj getSearchQuery() const {
        return _searchQuery.getOwned();
    }

    auto getTaskExecutor() const {
        return _taskExecutor;
    }

    void setCursor(executor::TaskExecutorCursor cursor) {
        _cursor.emplace(std::move(cursor));
        _dispatchedQuery = true;
    }

    boost::optional<long long> getMongotDocsRequested() const {
        return _mongotDocsRequested;
    }

    bool getPaginationFlag() const {
        return _requiresSearchSequenceToken;
    }
    /**
     * Calculate the number of documents needed to satisfy a user-defined limit. This information
     * can be used in a getMore sent to mongot.
     */
    boost::optional<long long> calcDocsNeeded();

    /**
     * If a cursor establishment phase was run and returned no documents, make sure we don't later
     * repeat the query to mongot.
     */
    void markCollectionEmpty() {
        _dispatchedQuery = true;
    }

    /**
     * Create a copy of this document source that can be given a different cursor from the original.
     * Copies everything necessary to make a mongot remote query, but does not copy the cursor.
     */
    boost::intrusive_ptr<DocumentSourceInternalSearchMongotRemote> copyForAlternateSource(
        executor::TaskExecutorCursor cursor,
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) {
        tassert(6635400, "newExpCtx should not be null", newExpCtx != nullptr);
        auto newStage = boost::intrusive_ptr<DocumentSourceInternalSearchMongotRemote>(
            static_cast<DocumentSourceInternalSearchMongotRemote*>(clone(newExpCtx).get()));
        newStage->setCursor(std::move(cursor));
        return newStage;
    }

    boost::optional<int> getIntermediateResultsProtocolVersion() {
        // If it turns out that this stage is not running on a sharded collection, we don't want
        // to send the protocol version to mongot. If the protocol version is sent, mongot will
        // generate unmerged metadata documents that we won't be set up to merge.
        if (!pExpCtx->needsMerge) {
            return boost::none;
        }
        return _metadataMergeProtocolVersion;
    }

    bool queryReferencesSearchMeta() {
        return _queryReferencesSearchMeta;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    auto isStoredSource() const {
        return _searchQuery.hasField(kReturnStoredSourceArg)
            ? _searchQuery[kReturnStoredSourceArg].Bool()
            : false;
    }

protected:
    /**
     * Helper serialize method that avoids making mongot call during explain from mongos.
     */
    Value serializeWithoutMergePipeline(const SerializationOptions& opts) const;

    virtual Value serialize(const SerializationOptions& opts) const override;

    /**
     * Inspects the cursor to see if it set any vars, and propogates their definitions to the
     * ExpressionContext. For now, we only expect SEARCH_META to be defined.
     */
    void tryToSetSearchMetaVar();

    virtual executor::TaskExecutorCursor establishCursor();

    virtual GetNextResult getNextAfterSetup();

    bool shouldReturnEOF();

    /**
     * This stage may need to merge the metadata it generates on the merging half of the pipeline.
     * Until we know if the merge needs to be done, we hold the pipeline containig the merging
     * logic here.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> _mergingPipeline;

    boost::optional<executor::TaskExecutorCursor> _cursor;

private:
    /**
     * Does some common setup and checks, then calls 'getNextAfterSetup()' if appropriate.
     */
    GetNextResult doGetNext() final;

    boost::optional<BSONObj> _getNext();

    /**
     * Helper function that determines whether the document source references the $$SEARCH_META
     * variable.
     */
    static bool hasReferenceToSearchMeta(const DocumentSource& ds) {
        std::set<Variables::Id> refs;
        ds.addVariableRefs(&refs);
        return Variables::hasVariableReferenceTo(refs,
                                                 std::set<Variables::Id>{Variables::kSearchMetaId});
    }

    const BSONObj _searchQuery;

    // If this is an explain of a $search at execution-level verbosity, then the explain
    // results are held here. Otherwise, this is an empty object.
    BSONObj _explainResponse;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    /**
     * Track whether either the stage or an earlier caller issues a mongot remote request. This
     * can be true even if '_cursor' is boost::none, which can happen if no documents are returned.
     */
    bool _dispatchedQuery = false;

    // Store the cursorId. We need to store it on the document source because the id on the
    // TaskExecutorCursor will be set to zero after the final getMore after the cursor is
    // exhausted.
    boost::optional<CursorId> _cursorId{boost::none};

    /**
     * Protocol version if it must be communicated via the search request.
     * If we are in a sharded environment but on a non-sharded collection we may have a protocol
     * version even though it should not be sent to mongot.
     */
    boost::optional<int> _metadataMergeProtocolVersion;

    long long _limit = 0;
    long long _docsReturned = 0;

    /**
     * Sort specification for the current query. Used to populate the $sortKey on mongod after
     * documents are returned from mongot.
     * boost::none if plan sharded search did not specify a sort.
     */
    boost::optional<BSONObj> _sortSpec;

    /**
     * Sort key generator used to populate $sortKey. Has a value iff '_sortSpec' has a value.
     */
    boost::optional<SortKeyGenerator> _sortKeyGen;

    /**
     * Flag indicating whether or not the total user pipeline references the $$SEARCH_META variable.
     * In sharded search, mongos will set this value send it to mongod; mongod should not try to
     * recompute this value since it may incorrectly think it doesn't need metadata if only the
     * merging pipeline (and not the shard's pipeline) references $$SEARCH_META.
     */
    bool _queryReferencesSearchMeta = true;

    /**
     * This will populate the docsRequested field of the cursorOptions document sent as part of the
     * command to mongot in the case where the query has an extractable limit that can guide the
     * number of documents that mongot returns to mongod.
     */
    boost::optional<long long> _mongotDocsRequested;

    bool _requiresSearchSequenceToken = false;
};

namespace search_meta {
/**
 * This function walks the pipeline and verifies that if there is a $search stage in a sub-pipeline
 * that there is no $$SEARCH_META access.
 */
void assertSearchMetaAccessValid(const Pipeline::SourceContainer& pipeline);

}  // namespace search_meta
}  // namespace mongo
