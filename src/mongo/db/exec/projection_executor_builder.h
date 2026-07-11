// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/util/modules.h"

#include <bitset>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
     * Indicates whether the projection executor is for inclusion only projection or not. The
     * semantics of this flag should be consistent with the same flag in the
     * 'InclusionProjectionExecutor'.
     */
    kNotInclusionOnly,

    /**
     * An internal value holding the total number of projection executor builder parameters.
     * New parameters must be added before the kNumParams sentinel.
     */
    kNumParams
};

using BuilderParamsBitSet = std::bitset<BuilderParams::kNumParams>;

/**
 * By default all projection executor builder parameters are turned on.
 *
 * TODO SERVER-113179: Remove external dependencies on this constant.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr auto kDefaultBuilderParams =
    BuilderParamsBitSet(~(1 << BuilderParams::kNumParams));

/**
 * Builds a projection execution tree from the given 'projection' and using the given projection
 * 'policies' by walking an AST tree starting at the root node stored within the 'projection'.
 * The 'params' parameter is used to pass various control parameters to alter the behaviour of the
 * projection executor builder.
 *
 * TODO SERVER-113179: Remove external dependencies on this constant.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::unique_ptr<ProjectionExecutor> buildProjectionExecutor(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const projection_ast::Projection* projection,
    ProjectionPolicies policies,
    BuilderParamsBitSet params);
}  // namespace mongo::projection_executor
