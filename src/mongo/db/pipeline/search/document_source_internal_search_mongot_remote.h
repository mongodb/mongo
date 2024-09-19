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
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
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

    DocumentSourceInternalSearchMongotRemote(InternalSearchMongotRemoteSpec spec,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             std::shared_ptr<executor::TaskExecutor> taskExecutor)
        : DocumentSource(kStageName, expCtx),
          _mergingPipeline(spec.getMergingPipeline().has_value()
                               ? mongo::Pipeline::parse(*spec.getMergingPipeline(), expCtx)
                               : nullptr),
          _spec(std::move(spec)),
          _taskExecutor(taskExecutor) {
        if (_spec.getSortSpec().has_value()) {
            _sortKeyGen.emplace(SortPattern{*_spec.getSortSpec(), pExpCtx}, pExpCtx->getCollator());
        }
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return getSearchDefaultConstraints();
    }

    const char* getSourceName() const override;

    DocumentSourceType getType() const override {
        return DocumentSourceType::kInternalSearchMongotRemote;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        // The desugaring of DocumentSourceSearch happens after sharded planning, so we should never
        // execute distributedPlanLogic here, instead it should be called against
        // DocumentSourceSearch.
        MONGO_UNREACHABLE_TASSERT(7815902);
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : pExpCtx;
        auto spec = InternalSearchMongotRemoteSpec::parseOwned(IDLParserContext(kStageName),
                                                               _spec.toBSON());
        return make_intrusive<DocumentSourceInternalSearchMongotRemote>(
            std::move(spec), expCtx, _taskExecutor);
    }

    const InternalSearchMongotRemoteSpec& getMongotRemoteSpec() const {
        return _spec;
    }

    BSONObj getSearchQuery() const {
        return _spec.getMongotQuery().getOwned();
    }

    auto getTaskExecutor() const {
        return _taskExecutor;
    }

    void setCursor(std::unique_ptr<executor::TaskExecutorCursor> cursor) {
        _cursor = std::move(cursor);
        _dispatchedQuery = true;
    }

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
        std::unique_ptr<executor::TaskExecutorCursor> cursor,
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
        return _spec.getMetadataMergeProtocolVersion();
    }

    bool queryReferencesSearchMeta() {
        return _spec.getRequiresSearchMetaCursor();
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    auto isStoredSource() const {
        auto storedSourceElem = _spec.getMongotQuery()[mongot_cursor::kReturnStoredSourceArg];
        return !storedSourceElem.eoo() && storedSourceElem.Bool();
    }

    void setDocsNeededBounds(DocsNeededBounds bounds) {
        // The bounds may have already been set when mongos walked the entire user pipeline. In that
        // case, we shouldn't override the existing bounds.
        if (!_spec.getDocsNeededBounds().has_value()) {
            _spec.setDocsNeededBounds(bounds);
        }
    }

    void setSearchIdLookupMetrics(
        std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
            searchIdLookupMetrics) {
        _searchIdLookupMetrics = std::move(searchIdLookupMetrics);
    }

    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
    getSearchIdLookupMetrics() {
        // Will be nullptr if query is stored source.
        return _searchIdLookupMetrics;
    }

protected:
    /**
     * Helper serialize method that avoids making mongot call during explain from mongos.
     */
    Value serializeWithoutMergePipeline(const SerializationOptions& opts) const;

    Value serialize(const SerializationOptions& opts) const override;

    /**
     * Inspects the cursor to see if it set any vars, and propogates their definitions to the
     * ExpressionContext. For now, we only expect SEARCH_META to be defined.
     */
    void tryToSetSearchMetaVar();

    virtual std::unique_ptr<executor::TaskExecutorCursor> establishCursor();

    virtual GetNextResult getNextAfterSetup();

    bool shouldReturnEOF();

    /**
     * This stage may need to merge the metadata it generates on the merging half of the pipeline.
     * Until we know if the merge needs to be done, we hold the pipeline containig the merging
     * logic here.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> _mergingPipeline;

    std::unique_ptr<executor::TaskExecutorCursor> _cursor;

private:
    /**
     * Does some common setup and checks, then calls 'getNextAfterSetup()' if appropriate.
     */
    GetNextResult doGetNext() final;

    boost::optional<BSONObj> _getNext();

    InternalSearchMongotRemoteSpec _spec;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    /**
     * Track whether either the stage or an earlier caller issues a mongot remote request. This
     * can be true even if '_cursor' is nullptr, which can happen if no documents are returned.
     */
    bool _dispatchedQuery = false;

    // Store the cursorId. We need to store it on the document source because the id on the
    // TaskExecutorCursor will be set to zero after the final getMore after the cursor is
    // exhausted.
    boost::optional<CursorId> _cursorId{boost::none};

    long long _docsReturned = 0;

    /**
     * Sort key generator used to populate $sortKey. Has a value iff '_sortSpec' has a value.
     */
    boost::optional<SortKeyGenerator> _sortKeyGen;

    /**
     * SearchIdLookupMetrics between MongotRemote & SearchIdLookup DocumentSources.
     * The state is shared between these two document sources because SearchIdLookup
     * computes the document id lookup success rate, and MongotRemote uses it to make decisions
     * about the batch size it requests for search responses.
     * Note, this pointer could be null, and must be set before use.
     */
    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
        _searchIdLookupMetrics = nullptr;
};

}  // namespace mongo
