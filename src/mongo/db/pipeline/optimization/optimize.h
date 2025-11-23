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

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace pipeline_optimization {
/**
 * Enabling the disablePipelineOptimization fail point will stop the aggregate command from
 * attempting to optimize the pipeline or the pipeline stages. Neither DocumentSource::optimizeAt()
 * nor DocumentSource::optimize() will be attempted.
 */
extern FailPoint disablePipelineOptimization;

/**
 * Modifies the pipeline, optimizing it by combining and swapping stages.
 */
MONGO_MOD_NEEDS_REPLACEMENT void optimizePipeline(Pipeline& pipeline);

/**
 * Modifies the container, optimizes each stage individually.
 */
MONGO_MOD_NEEDS_REPLACEMENT void optimizeEachStage(const ExpressionContext&,
                                                   DocumentSourceContainer* container);

/**
 * Modifies the container, optimizing it by combining, swapping, dropping and/or inserting
 * stages. If 'itr' is given, optimizes the pipeline starting from the stage that 'itr' points to.
 */
void optimizeContainer(const ExpressionContext&,
                       DocumentSourceContainer* container,
                       boost::optional<DocumentSourceContainer::iterator> itr = {});

/**
 * Optimize the given pipeline after the stage that 'itr' points to.
 *
 * Returns a valid iterator that points to the new "end of the pipeline": i.e., the stage that
 * comes after 'itr' in the newly optimized pipeline.
 */
DocumentSourceContainer::iterator optimizeEndOfPipeline(const ExpressionContext&,
                                                        DocumentSourceContainer::iterator itr,
                                                        DocumentSourceContainer* container);

/*
 * Helper to optimize and validate pipelines. This helper is used by stages that execute
 * subpipelines (lookup, graphLookup, unionWith), and **must** be called before we execute the
 * subpipeline.
 */
inline void optimizeAndValidatePipeline(Pipeline* pipeline) {
    tassert(10313300, "Expected pipeline to optimize", pipeline);
    optimizePipeline(*pipeline);
    pipeline->validateCommon(true /* alreadyOptimized */);
}
}  // namespace pipeline_optimization
}  // namespace mongo
