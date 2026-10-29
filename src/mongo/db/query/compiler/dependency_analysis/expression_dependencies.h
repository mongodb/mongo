// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/modules.h"

#include <set>

namespace mongo::expression {

/**
 * Add the dependencies required by 'expr' to 'deps', including any metadata or field references.
 */
void addDependencies(const Expression* expr, DepsTracker* deps);

/**
 * Convenience wrapper around addDependencies.
 */
DepsTracker getDependencies(const Expression* expr);

/**
 * Append the variables referred to by 'expr' to the set 'refs', without clearing any pre-existing
 * references. Should not include $$ROOT or field path expressions.
 */
void addVariableRefs(const Expression* expr, std::set<Variables::Id>* refs);

}  // namespace mongo::expression
