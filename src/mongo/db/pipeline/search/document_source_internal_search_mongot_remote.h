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
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/stacktrace.h"

#include <queue>

namespace mongo {

struct InternalSearchMongotRemoteSharedState {
    // TODO SERVER-94874: This does not need to be shared anymore when the ticket is done.
    std::unique_ptr<executor::TaskExecutorCursor> _cursor;
};

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
        constraints.setConstraintsForNoInputSources();
        // All search stages are unsupported on timeseries collections.
        constraints.canRunOnTimeseries = false;
        return constraints;
    }

    DocumentSourceInternalSearchMongotRemote(InternalSearchMongotRemoteSpec spec,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             std::shared_ptr<executor::TaskExecutor> taskExecutor);

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        return getSearchDefaultConstraints();
    }

    const char* getSourceName() const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        // The desugaring of DocumentSourceSearch happens after sharded planning, so we should never
        // execute distributedPlanLogic here, instead it should be called against
        // DocumentSourceSearch.
        MONGO_UNREACHABLE_TASSERT(7815902);
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : getExpCtx();
        auto spec = InternalSearchMongotRemoteSpec::parseOwned(_spec.toBSON(),
                                                               IDLParserContext(kStageName));
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
        _sharedState->_cursor = std::move(cursor);
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
        if (!getExpCtx()->getNeedsMerge()) {
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

    auto hasScoreDetails() const {
        auto scoreDetailsElem = _spec.getMongotQuery()[mongot_cursor::kScoreDetailsFieldName];
        return !scoreDetailsElem.eoo() && scoreDetailsElem.Bool();
    }

    auto hasReturnScope() const {
        auto returnScopeElem = _spec.getMongotQuery()[mongot_cursor::kReturnScopeArg];
        return !returnScopeElem.eoo() && returnScopeElem.isABSONObj();
    }

    auto hasSearchRootDocumentId() const {
        return isStoredSource() && hasReturnScope();
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

    /**
     * Helper function to add the "mergingPipeline" field to the serialization if it's needed.
     */
    Value addMergePipelineIfNeeded(Value innerSpecVal, const SerializationOptions& opts) const;

    Value serialize(const SerializationOptions& opts) const override;

    /**
     * This stage may need to merge the metadata it generates on the merging half of the pipeline.
     * Until we know if the merge needs to be done, we hold the pipeline containing the merging
     * logic here.
     */
    std::unique_ptr<Pipeline> _mergingPipeline;

    std::shared_ptr<InternalSearchMongotRemoteSharedState> _sharedState;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSearchMongotRemoteToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceSearchMetaToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    InternalSearchMongotRemoteSpec _spec;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

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
