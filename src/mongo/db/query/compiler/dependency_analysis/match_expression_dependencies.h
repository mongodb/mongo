// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/modules.h"

#include <set>

namespace mongo::dependency_analysis {

/**
 * Add the dependencies required by 'expr' to 'deps', including any metadata or field references.
 */
void addDependencies(const MatchExpression* expr, DepsTracker* deps);

/**
 * Append the variables referred to by 'expr' to the set 'refs', without clearing any pre-existing
 * references. Should not include $$ROOT or field path expressions.
 */
void addVariableRefs(const MatchExpression* expr, std::set<Variables::Id>* refs);

}  // namespace mongo::dependency_analysis
