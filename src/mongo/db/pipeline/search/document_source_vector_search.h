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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Interface used to retrieve the execution stats of explain().
 *
 * Used to decouple the execution code from the optimization one (so the optimization code does not
 * need a TaskExecutorCursor for example).
 * TODO SERVER-107930: It can go away once VectorSearchStage::getExplainOutput() is implemented.
 */
class DSVectorSearchExecStatsWrapper {
public:
    class StatsProvider {
    public:
        virtual boost::optional<BSONObj> getStats() = 0;
        virtual ~StatsProvider() = default;
    };

    /**
     * Retrieves the execution statistics.
     */
    boost::optional<BSONObj> getExecStats() {
        if (!_provider) {
            return boost::none;
        }
        return _provider->getStats();
    }

    void setStatsProvider(std::unique_ptr<StatsProvider> provider) {
        _provider = std::move(provider);
    }

private:
    std::unique_ptr<StatsProvider> _provider{nullptr};
};

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
    static constexpr StringData kViewFieldName = "view"_sd;
    static constexpr StringData kReturnStoredSourceFieldName = "returnStoredSource"_sd;

    DocumentSourceVectorSearch(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               std::shared_ptr<executor::TaskExecutor> taskExecutor,
                               BSONObj originalSpec);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    std::list<boost::intrusive_ptr<DocumentSource>> desugar();

    const char* getSourceName() const override {
        return kStageName.data();
    }

    bool isStoredSource() const {
        // If the user specifies storedSource: true and the knob is off, we will not error and
        // simply return false.
        bool isVectorSearchStoredSourceEnabled =
            ServerParameterSet::getClusterParameterSet()
                ->get<ClusterParameterWithStorage<InternalVectorSearchStoredSource>>(
                    "internalVectorSearchStoredSource")
                ->getValue(getExpCtx()->getNamespaceString().tenantId())
                .getEnabled();
        return isVectorSearchStoredSourceEnabled
            ? _originalSpec.getBoolField(kReturnStoredSourceFieldName)
            : false;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        DistributedPlanLogic logic;
        logic.shardsStage = this;
        if (_limit) {
            logic.mergingStages = {DocumentSourceLimit::create(getExpCtx(), *_limit)};
        }
        logic.mergeSortPattern = kSortSpec;
        return logic;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        // This stage doesn't currently support tracking field dependencies since mongot is
        // responsible for determining what fields to return. We do need to track metadata
        // dependencies though, so downstream stages know they are allowed to access
        // "vectorSearchScore" metadata.
        // TODO SERVER-101100 Implement logic for dependency analysis.

        deps->setMetadataAvailable(DocumentMetadataFields::kVectorSearchScore);
        return DepsTracker::State::NOT_SUPPORTED;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : getExpCtx();
        return make_intrusive<DocumentSourceVectorSearch>(
            expCtx, _taskExecutor, _originalSpec.copy());
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.setConstraintsForNoInputSources();
        // All search stages are unsupported on timeseries collections.
        constraints.canRunOnTimeseries = false;
        return constraints;
    }

protected:
    Value serialize(const SerializationOptions& opts) const override;

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) override;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceVectorSearchToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    // Initialize metrics related to the $vectorSearch stage on the OpDebug object.
    void initializeOpDebugVectorSearchMetrics();

    /**
     * Attempts a pipeline optimization that removes a $sort stage that comes after the output of
     * of mongot, if the resulting documents from mongot are sorted by the same criteria as the
     * $sort ('vectorSearchScore').
     *
     * Also, this optimization only applies to cases where the $sort comes directly after this
     * stage.
     * TODO SERVER-96068 generalize this optimization to cases where any number of stages that
     * preserve sort order come between this stage and the sort.
     *
     * Returns a pair of the iterator to return to the optimizer, and a bool of whether or not the
     * optimization was successful. If optimization was successful, the container will be modified
     * appropriately.
     */
    std::pair<DocumentSourceContainer::iterator, bool> _attemptSortAfterVectorSearchOptimization(
        DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

    std::unique_ptr<MatchExpression> _filterExpr;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Set when the execution stage is created.
    // TODO SERVER-107930: Remove it when SourceVectorSearch::getExplainOutput() is
    // implemented.
    std::weak_ptr<DSVectorSearchExecStatsWrapper> _execStatsWrapper;

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
