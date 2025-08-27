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
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
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

    DocumentSourceSearch(const boost::intrusive_ptr<ExpressionContext> expCtx,
                         InternalSearchMongotRemoteSpec spec)
        : DocumentSource(kStageName, expCtx), _spec(std::move(spec)) {}

    const char* getSourceName() const override;
    StageConstraints constraints(PipelineSplitState pipeState) const override;
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}
    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : getExpCtx();
        return make_intrusive<DocumentSourceSearch>(expCtx, _spec);
    }

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

    std::list<boost::intrusive_ptr<DocumentSource>> desugar();

    BSONObj getSearchQuery() const {
        return _spec.getMongotQuery().getOwned();
    }

    const InternalSearchMongotRemoteSpec& getMongotRemoteSpec() const {
        return _spec;
    }

    boost::optional<long long> getLimit() const {
        return _spec.getLimit().has_value() ? boost::make_optional<long long>(*_spec.getLimit())
                                            : boost::none;
    }

    bool getSearchPaginationFlag() {
        return _spec.getRequiresSearchSequenceToken();
    }

    boost::optional<int> getIntermediateResultsProtocolVersion() const {
        // If it turns out that this stage is not running on a sharded collection, we don't want
        // to send the protocol version to mongot. If the protocol version is sent, mongot will
        // generate unmerged metadata documents that we won't be set up to merge.
        if (!getExpCtx()->getNeedsMerge()) {
            return boost::none;
        }
        return _spec.getMetadataMergeProtocolVersion();
    }

    boost::optional<BSONObj> getSortSpec() const {
        return _spec.getSortSpec();
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

    void setCursor(std::unique_ptr<executor::TaskExecutorCursor> cursor) {
        _cursor = std::move(cursor);
    }

    std::unique_ptr<executor::TaskExecutorCursor> getCursor() {
        return std::move(_cursor);
    }

    void setMetadataCursor(std::unique_ptr<executor::TaskExecutorCursor> cursor) {
        _metadataCursor = std::move(cursor);
    }

    std::unique_ptr<executor::TaskExecutorCursor> getMetadataCursor() {
        return std::move(_metadataCursor);
    }

    void setDocsNeededBounds(DocsNeededBounds bounds) {
        _spec.setDocsNeededBounds(bounds);
    }

private:
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) override;

    // Holds all the planning information for the command's eventual mongot request.
    InternalSearchMongotRemoteSpec _spec;

    // An unique id of search stage in the pipeline, currently it is hard coded to 0 because we can
    // only have one search stage and sub-pipelines are not in the same PlanExecutor.
    // We should assign unique ids when we have everything in a single PlanExecutorSBE.
    size_t _remoteCursorId{0};

    // The mongot data and metadata cursors for search. We establish the cursors before query
    // planning, use this object to temporarily store the cursors, and will transfer the cursors to
    // corresponding SBE executors when build PlanExecutorSBE.
    std::unique_ptr<executor::TaskExecutorCursor> _cursor;
    std::unique_ptr<executor::TaskExecutorCursor> _metadataCursor;
    boost::optional<BSONObj> _remoteCursorVars;
};

}  // namespace mongo
