// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_sharding.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

namespace mongo {

class SimpleMemoryUsageTracker;

namespace exec::expression {

Value evaluate(const ExpressionInternalOwningShard& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);
Value evaluate(const ExpressionInternalIndexKey& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx);

}  // namespace exec::expression
}  // namespace mongo
