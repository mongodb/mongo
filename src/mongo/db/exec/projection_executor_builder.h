/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <bitset>

#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/query/projection_ast.h"

namespace mongo::projection_executor {
/**
 * Various parameters which control the behaviour of the projection executor builder.
 */
enum BuilderParams : char {
    /**
     * Indicates whether the 'optimize()' method needs to be called on the newly created executor
     * before returning it to the caller.
     *
     * Start from 1 so that these values can be stored in a bitset.
     */
    kOptimizeExecutor = 1,

    /**
     * Pass this parameter to the projection executor builder if fast-path projection execution is
     * allowed. The caller can disable the fast-path, but is never required to do so for correctness
     * since it will be disabled internally for correctness as needed.
     *
     * Note that this setting is just a hint to the projection executor builder and does not
     * guarantee that the fast-path projection executor will be created. The fast path executor will
     * only be created for certain types of projections, and the fast path may only be used for
     * Documents which are trivially convertible to BSON.
     *
     * Moreover, even if the projection executor is created with the fast-path enabled, the decision
     * whether to use the fast-path or not is made on document-by-document basis. So, if applying a
     * fast-path projection is not possible, the executor will fall back to the default
     * implementation.
     */
    kAllowFastPath,

    /**
     * An internal value holding the total number of projection executor builder parameters.
     * New parameters must be added before the kNumParams sentinel.
     */
    kNumParams
};

using BuilderParamsBitSet = std::bitset<BuilderParams::kNumParams>;

/**
 * By default all projection executor builder parameters are turned on.
 */
static constexpr auto kDefaultBuilderParams =
    BuilderParamsBitSet(~(1 << BuilderParams::kNumParams));

/**
 * Builds a projection execution tree from the given 'projection' and using the given projection
 * 'policies' by walking an AST tree starting at the root node stored within the 'projection'.
 * The 'params' parameter is used to pass various control parameters to alter the behaviour of the
 * projection executor builder.
 */
std::unique_ptr<ProjectionExecutor> buildProjectionExecutor(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const projection_ast::Projection* projection,
    const ProjectionPolicies policies,
    BuilderParamsBitSet params);
}  // namespace mongo::projection_executor
