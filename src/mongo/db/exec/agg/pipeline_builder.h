// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class Pipeline;

namespace exec::agg {

/**
 * Builds and returns a query execution pipeline from the given document source pipeline.
 * Expects that the document source is 'frozen' for modifications.
 * Raises a tassert when called with an empty 'pipeline' object, i.e. a pipeline that contains 0
 * 'DocumentSource's, because empty pipelines cannot be executed.
 * TODO SERVER-105562: Return the resulting pipeline by value.
 * TODO SERVER-112776: Remove 'data_movement' dependency on this function.
 * TODO SERVER-112777: Remove 'atlas_streams' dependency on this function.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::unique_ptr<exec::agg::Pipeline> buildPipeline(
    const mongo::Pipeline& pipeline);

}  // namespace exec::agg
}  // namespace mongo
