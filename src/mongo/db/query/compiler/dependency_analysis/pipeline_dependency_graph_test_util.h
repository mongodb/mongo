// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "src/mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

namespace mongo::pipeline::dependency_graph {
/**
 * Runs the given assertions before and after rebuilding the graph from every stage.
 */
template <typename F>
inline void recomputeAndAssert(DependencyGraph& graph, const Pipeline& pipeline, F&& func) {
    const auto& sources = pipeline.getSources();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        func();
        graph.recompute_forTest(it);
    }
    func();
}
}  // namespace mongo::pipeline::dependency_graph
