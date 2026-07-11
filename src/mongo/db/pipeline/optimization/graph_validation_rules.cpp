// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/optimization/graph_validation_rules.h"

#include "mongo/db/pipeline/document_source_internal_assert_data_assumptions.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

namespace mongo::pipeline_optimization {

void removeArraynessValidationStages(Pipeline& pipeline) {
    auto& sources = pipeline.getSources();
    for (auto it = sources.begin(); it != sources.end();) {
        if (dynamic_cast<DocumentSourceInternalAssertDataAssumptions*>(it->get())) {
            it = sources.erase(it);
        } else {
            ++it;
        }
    }
}

void insertArraynessValidationStages(Pipeline& pipeline) {
    // Skip insertion during explain operations since these are runtime validation stages only.
    if (pipeline.getContext()->getExplain()) {
        return;
    }

    auto& sources = pipeline.getSources();
    if (sources.empty()) {
        return;
    }

    // Collect validation stages to insert (stage position + paths to validate)
    struct ValidationInfo {
        size_t insertPosition;
        std::set<FieldPath> nonArrayPaths;
    };
    std::vector<ValidationInfo> validationsToInsert;

    pipeline::dependency_graph::DependencyGraphContext ctx(*pipeline.getContext(),
                                                           pipeline.getSources());
    auto& graph = ctx.getGraph();

    size_t stageIndex = 0;
    for (const auto& stage : sources) {
        DepsTracker deps;
        stage->getDependencies(&deps);

        // Never inject a validation stage at the very front of the pipeline. The leading stage
        // reads directly from the collection and may be pulled into the query executor as part of
        // the find layer (e.g. a leading $match, $sort, or $geoNear). Injecting a validation stage
        // ahead of it makes the validation stage the pipeline front and defeats that pushdown. This
        // costs no dependency-graph coverage: for the leading stage, canPathBeArray() is a direct
        // passthrough to the collection's path-arrayness metadata, not the graph's
        // stage-transformation inference that this validation is meant to exercise.
        if (stageIndex == 0 || deps.needWholeDocument || deps.fields.empty()) {
            ++stageIndex;
            continue;
        }

        std::set<FieldPath> nonArrayPaths;
        for (const auto& fieldPath : deps.fields) {
            if (!graph.canPathBeArray(stage.get(), fieldPath)) {
                nonArrayPaths.insert(FieldPath(fieldPath));
            }
        }

        if (!nonArrayPaths.empty()) {
            validationsToInsert.push_back({stageIndex, std::move(nonArrayPaths)});
        }

        ++stageIndex;
    }

    // Insert validation stages in reverse order to maintain correct positions
    if (!validationsToInsert.empty()) {
        for (auto it = validationsToInsert.rbegin(); it != validationsToInsert.rend(); ++it) {
            auto validationStage = DocumentSourceInternalAssertDataAssumptions::create(
                pipeline.getContext(), std::move(it->nonArrayPaths));
            pipeline.addSourceAtPosition(validationStage, it->insertPosition);
        }
    }
}

}  // namespace mongo::pipeline_optimization

