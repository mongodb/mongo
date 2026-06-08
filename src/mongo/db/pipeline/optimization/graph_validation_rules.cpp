/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

        if (deps.needWholeDocument || deps.fields.empty()) {
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

