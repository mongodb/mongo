// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

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

/**
 * Produces a formatted string representation of the pipeline for golden testing.
 */
std::string toString(const Pipeline& pipeline);

/**
 * Parses the given json string into a pipeline.
 */
std::unique_ptr<Pipeline> parsePipeline(const std::string& inputPipeJson);
}  // namespace mongo::pipeline::dependency_graph
