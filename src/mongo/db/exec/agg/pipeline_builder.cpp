// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/pipeline_builder.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/pipeline.h"

#include <iterator>

namespace mongo::exec::agg {

std::unique_ptr<exec::agg::Pipeline> buildPipeline(const mongo::Pipeline& pipeline) {
    // TODO SERVER-105521: Remove the following assertion once pipeline.isFrozen() is replaced
    // with FrozenPipeline class.
    tassert(
        10706500, "expecting pipeline frozen for modifications as an input", pipeline.isFrozen());
    Pipeline::StageContainer stages;
    const auto& documentSources = pipeline.getSources();

    // 'Stitch' stages together in the given order — a stage becomes the 'source' for the
    // following stage. A DocumentSource may expand to N stages and the last stage of one expansion
    // becomes the source of the first stage of the next.
    auto buildAndInsert = [&](const boost::intrusive_ptr<DocumentSource>& ds) {
        auto expansion = buildStages(ds);
        if (!stages.empty())
            stitchStage(*expansion.front(), stages.back().get());
        for (size_t i = 1; i < expansion.size(); i++)
            stitchStage(*expansion[i], expansion[i - 1].get());
        stages.insert(stages.end(),
                      std::make_move_iterator(expansion.begin()),
                      std::make_move_iterator(expansion.end()));
    };

    try {
        if (MONGO_likely(!documentSources.empty())) {
            // Lower bound; 1:N expansions may exceed this.
            stages.reserve(documentSources.size());
            for (const auto& ds : documentSources)
                buildAndInsert(ds);
        }
    } catch (...) {
        // Dispose the stages already created if an exception occurs.
        // TODO SERVER-109935: Not needed once each stage auto-disposes itself in the destructor.
        if (!stages.empty()) {
            stages.back()->dispose();
        }

        throw;
    }

    return std::make_unique<exec::agg::Pipeline>(std::move(stages), pipeline.getContext());
}

}  // namespace mongo::exec::agg
