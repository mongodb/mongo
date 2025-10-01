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

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"

namespace mongo::join_ordering {
/** Represent an aggregation pipeline for join optimization. It takes a pipeline and parses a join
 * graph from it.
 */
struct AggJoinModel {
    /** Returns false if we are sure that the pipeline cannot be optimized with join reordering,
     * true if we should try.
     */
    static bool canOptimizeWithJoinReordering(const std::unique_ptr<Pipeline>& pipeline);

    AggJoinModel(std::unique_ptr<Pipeline> pipeline);

    JoinGraph graph;

    std::vector<ResolvedPath> resolvedPaths;

    // Stages extracted for join optimization and pushed down to CanonicalQueries in JoinGraph.
    std::unique_ptr<Pipeline> prefix;

    // Ramaining stages not extracted for join optiomization.
    std::unique_ptr<Pipeline> suffix;

    /**Serializes the Aggregation Join Model to BSON.*/
    BSONObj toBSON() const;

    /** Converts the Aggregation Join Model to a JSON string. If 'pretty' is true the output JSON
     * string is idented.
     */
    std::string toString(bool pretty) const {
        return toBSON().jsonString(/*format*/ ExtendedCanonicalV2_0_0, pretty);
    }

private:
    void build();

    /** Creates a JoinGraph node for the pipeline's base collection.
     */
    boost::optional<NodeId> makeBaseNode();
};
}  // namespace mongo::join_ordering
