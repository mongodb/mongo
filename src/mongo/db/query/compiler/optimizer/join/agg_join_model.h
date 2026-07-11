// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Defines size limits for graphs during build and extend operations.
 */
class AggModelBuildParams {
public:
    JoinGraphBuildParams joinGraphBuildParams;
    size_t maxNumberNodesConsideredForImplicitEdges;
};

/** Represent an aggregation pipeline for join optimization. It takes a pipeline and parses a join
 * graph from it.
 */
class AggJoinModel {
public:
    /** Returns false if we are sure that the pipeline cannot be optimized with join reordering,
     * true if we should try.
     */
    static bool pipelineEligibleForJoinReordering(const Pipeline& pipeline);

    /**
     * Factory function to construct an AggJoinModel instance from a 'pipeline'. If construction
     * succeeds, ownership of the pipeline will be transferred to the 'AggJoinModel'. If
     * construction fails, a status is returned and the pipeline remains unmodified.
     * * `maxNumberNodesConsideredForImplicitEdges` is the maximum number of nodes allowed in a
     * connected component to be used for implicit edge finding.
     */
    static StatusWith<AggJoinModel> constructJoinModel(const Pipeline& pipeline,
                                                       AggModelBuildParams buildParams);

    AggJoinModel(JoinGraph graph,
                 std::vector<ResolvedPath> resolvedPaths,
                 std::unique_ptr<Pipeline> prefix,
                 std::unique_ptr<Pipeline> suffix,
                 std::vector<BSONObj> accessPathsBackingBson,
                 boost::intrusive_ptr<ExpressionContext> joinExpCtx)
        : graph{std::move(graph)},
          resolvedPaths{std::move(resolvedPaths)},
          prefix{std::move(prefix)},
          suffix{std::move(suffix)},
          accessPathsBackingBson{std::move(accessPathsBackingBson)},
          _joinExpCtx{std::move(joinExpCtx)} {}

    AggJoinModel(AggJoinModel&& other)
        : graph{std::move(other.graph)},
          resolvedPaths{std::move(other.resolvedPaths)},
          prefix{std::move(other.prefix)},
          suffix{std::move(other.suffix)},
          accessPathsBackingBson{std::move(other.accessPathsBackingBson)},
          _joinExpCtx{std::move(other._joinExpCtx)} {}

    AggJoinModel& operator=(AggJoinModel&& other) {
        graph = std::move(other.graph);
        resolvedPaths = std::move(other.resolvedPaths);
        prefix = std::move(other.prefix);
        suffix = std::move(other.suffix);
        accessPathsBackingBson = std::move(other.accessPathsBackingBson);
        _joinExpCtx = std::move(other._joinExpCtx);
        return *this;
    }

    /** Serializes the Aggregation Join Model to BSON. */
    BSONObj toBSON() const;

    /** Converts the Aggregation Join Model to a JSON string. */
    std::string toString(bool pretty) const {
        return toBSON().jsonString(/*format*/ ExtendedCanonicalV2_0_0, pretty);
    }

    const JoinGraph& getGraph() const {
        return graph;
    }
    const std::vector<ResolvedPath>& getResolvedPaths() const {
        return resolvedPaths;
    }
    Pipeline* getPrefix() const {
        return prefix.get();
    }
    Pipeline* getSuffix() const {
        return suffix.get();
    }
    /**
     * The ExpressionContext used throughout join optimization. It is a clone of the original
     * pipeline's context, kept separate so that a fallback leaves the original untouched. All
     * non-array path learnings from join-predicate eligibility checks accumulate here.
     */
    const boost::intrusive_ptr<ExpressionContext>& getJoinExpCtx() const {
        return _joinExpCtx;
    }
    std::unique_ptr<Pipeline> releaseSuffix() {
        return std::move(suffix);
    }
    const std::vector<BSONObj>& getAccessPathsBackingBson() const {
        return accessPathsBackingBson;
    }

private:
    JoinGraph graph;

    std::vector<ResolvedPath> resolvedPaths;

    // Stages extracted for join optimization and pushed down to CanonicalQueries in JoinGraph.
    std::unique_ptr<Pipeline> prefix;

    // Remaining stages not extracted for join optimization.
    std::unique_ptr<Pipeline> suffix;

    std::vector<BSONObj> accessPathsBackingBson;

    // Clone of the original pipeline's ExpressionContext used throughout join optimization.
    boost::intrusive_ptr<ExpressionContext> _joinExpCtx;
};

}  // namespace mongo::join_ordering
