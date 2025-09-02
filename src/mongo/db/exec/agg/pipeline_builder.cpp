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

#include "mongo/db/exec/agg/pipeline_builder.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo::exec::agg {

std::unique_ptr<exec::agg::Pipeline> buildPipeline(const mongo::Pipeline& pipeline) {
    // TODO SERVER-102417: Remove the following assertion once all document sources have been
    // splitted.
    tassert(
        10706500, "expecting pipeline frozen for modifications as an input", pipeline.isFrozen());
    Pipeline::StageContainer stages;
    const auto& documentSources = pipeline.getSources();

    try {
        if (MONGO_likely(!documentSources.empty())) {
            stages.reserve(documentSources.size());
            auto it = documentSources.cbegin();
            stages.push_back(buildStage(*it));
            for (++it; it != documentSources.cend(); ++it) {
                // 'Stitch' stages together in the given order - a stage becomes the 'source' for
                // the following stage.
                stages.push_back(buildStageAndStitch(*it, stages.back()));
            }
        }
    } catch (...) {
        // Dispose the stages already created if an exception occurs.
        // TODO SERVER-109935: Not needed once each stage auto-disposes itself in the destructor
        // unless dismissDisposal() is called.
        if (!stages.empty()) {
            stages.back()->dispose();
        }

        throw;
    }

    return std::make_unique<exec::agg::Pipeline>(std::move(stages), pipeline.getContext());
}

}  // namespace mongo::exec::agg
