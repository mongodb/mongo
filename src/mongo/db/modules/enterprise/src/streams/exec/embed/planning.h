/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 */

#pragma once

#include "mongo/db/modules/enterprise/src/streams/exec/context.h"
#include "mongo/db/modules/enterprise/src/streams/exec/embed/embed_operator.h"
#include "mongo/db/modules/enterprise/src/streams/exec/operator.h"
#include "mongo/db/modules/enterprise/src/streams/exec/stages_gen.h"
#include "mongo/db/pipeline/expression_context.h"

#include <memory>

namespace mongo::streams::embed {

/**
 * Build an EmbedOperator from a parsed $embed stage spec. Called from the central
 * Planner's stage dispatcher.
 *
 * Resolves the model.connectionName against the connection registry (must be one
 * of the supported embedding provider types), validates batch/cache config, and
 * compiles the input expression against `ctx->expCtx()`.
 */
std::unique_ptr<Operator> makeEmbedOperator(Context* ctx, const EmbedStageSpec& spec);

}  // namespace mongo::streams::embed
