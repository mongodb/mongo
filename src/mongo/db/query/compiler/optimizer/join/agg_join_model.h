/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
                 std::vector<BSONObj> accessPathsBackingBson)
        : graph{std::move(graph)},
          resolvedPaths{std::move(resolvedPaths)},
          prefix{std::move(prefix)},
          suffix{std::move(suffix)},
          accessPathsBackingBson{std::move(accessPathsBackingBson)} {}

    AggJoinModel(AggJoinModel&& other)
        : graph{std::move(other.graph)},
          resolvedPaths{std::move(other.resolvedPaths)},
          prefix{std::move(other.prefix)},
          suffix{std::move(other.suffix)},
          accessPathsBackingBson{std::move(other.accessPathsBackingBson)} {}

    AggJoinModel& operator=(AggJoinModel&& other) {
        graph = std::move(other.graph);
        resolvedPaths = std::move(other.resolvedPaths);
        prefix = std::move(other.prefix);
        suffix = std::move(other.suffix);
        accessPathsBackingBson = std::move(other.accessPathsBackingBson);
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
};

}  // namespace mongo::join_ordering
