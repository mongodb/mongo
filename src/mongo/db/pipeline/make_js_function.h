// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/javascript_execution.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

/**
 * Parses and returns an executable Javascript function.
 */
ScriptingFunction makeJsFunc(ExpressionContext* expCtx, const std::string& func);

}  // namespace mongo
