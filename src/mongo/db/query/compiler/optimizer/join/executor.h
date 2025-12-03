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
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

struct JoinReorderedExecutorResult {
    // Executor for pushed-down & reordered SBE prefix.
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> executor;
    // Model describing the join graph extracted from a pipeline and the DocumentSource suffix that
    // still needs to be attached to the executor.
    AggJoinModel model;
};

/**
 * Attempts to apply join optimization to the given aggregation, but if it fails to extract a join
 * model, falls back to preparing executors for the pipeline in the normal way.
 */
StatusWith<JoinReorderedExecutorResult> getJoinReorderedExecutor(
    const MultipleCollectionAccessor& mca,
    const Pipeline& pipeline,
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx);
}  // namespace mongo::join_ordering
