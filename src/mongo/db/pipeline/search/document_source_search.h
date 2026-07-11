// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Search);
class SearchLiteParsed final : public LiteParsedSearchStage<SearchLiteParsed> {
public:
    SearchLiteParsed(const BSONElement& originalBson, NamespaceString nss)
        : LiteParsedSearchStage(originalBson, std::move(nss)) {}

    static std::unique_ptr<SearchLiteParsed> parse(const NamespaceString& nss,
                                                   const BSONElement& spec,
                                                   const LiteParserOptions& options) {
        return std::make_unique<SearchLiteParsed>(spec, nss);
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<SearchStageParams>(_originalBson);
    }

    // $search produces $sortKey metadata.
    bool isRankedStage() const final {
        return true;
    }

    // $search produces $searchScore metadata.
    bool isScoredStage() const final {
        return true;
    }

    // $search produces scoreDetails metadata when the user requests it via the mongotQuery.
    bool isScoreDetailsStage() const final {
        return hasScoreDetails();
    }

    // $search is not a selection stage when returnStoredSource is true since it might have an
    // implicit projection applied.
    bool isSelectionStage() const final {
        return !hasReturnStoredSource();
    }
};

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
    static constexpr std::string_view kStageName = "$search"sv;

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
        : DocumentSource(kStageName,
                         expCtx,
                         [&]() -> SortPattern {
                             if (spec.getSortSpec().has_value()) {
                                 return SortPattern(spec.getSortSpec()->getOwned(), expCtx);
                             }
                             SortPattern::SortPatternPart part;
                             part.isAscending = false;
                             part.expression = make_intrusive<ExpressionMeta>(
                                 expCtx.get(), DocumentMetadataFields::MetaType::kSearchScore);
                             return SortPattern({std::move(part)});
                         }()),
          _spec(std::move(spec)) {}

    std::string_view getSourceName() const override;
    StageConstraints constraints(PipelineSplitState pipeState) const override;
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}
    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    bool providesSortKeyMetadata() const override {
        return true;
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

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

private:
    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

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
