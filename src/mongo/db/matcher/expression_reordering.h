// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
class MatchExpression;
class OperationContext;

/**
 * Enables dynamic reordering of the child predicates of every AND/OR/NOR node in 'expr', so that
 * the order of the subpredicates can change at runtime based on their observed selectivity: the
 * child that most often terminates evaluation early is moved to the front.
 *
 * Reordering is on by default and can be turned off with the
 * 'internalQueryEnableChangeStreamMatchExpressionReordering' server parameter, which this function
 * consults. This is the only place the parameter is read, so turning it off affects expressions set
 * up from that point on - a cursor that is already running keeps whatever it was given.
 *
 * A null 'expr' or null 'opCtx' is accepted and leaves reordering disabled.
 */
void allowReordering(OperationContext* opCtx, MatchExpression* expr);

}  // namespace mongo
