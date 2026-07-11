// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

#include <variant>

namespace mongo::query_settings::query_settings_details {

/**
 * The query settings lifecycle state for an operation. The active alternative encodes the current
 * phase:
 *
 * NotStarted    - No query-settings-eligible command has begun on this operation: pre-dispatch
 *                 reads, PQS-unsupported commands, and internal operations that never dispatch a
 *                 command. Reads see empty settings and knob reads are unrestricted.
 * Pending       - An eligible command has begun ('QuerySettingsCommandHooks') but query settings
 *                 have not been resolved yet. Reading 'forOp()' would observe stale values, so it
 *                 asserts.
 * Empty         - Resolution ran and determined no query settings apply; 'forOp()' returns empty.
 * QuerySettings - Resolution ran and installed query settings; the alternative holds the outcome.
 *
 * The only transitions are 'NotStarted' -> 'Pending' (command dispatch marks a PQS-supported
 * command via 'QuerySettingsCommandHooks') and 'Pending' -> 'Empty' | 'QuerySettings' (query
 * settings resolution); each happens at most once per operation.
 */
struct NotStarted {};
struct Pending {};
struct Empty {};

using QuerySettingsOperationState = std::variant<NotStarted, Pending, Empty, QuerySettings>;

/**
 * Returns the mutable query settings lifecycle state for 'opCtx'.
 */
QuerySettingsOperationState& getQuerySettingsStateForOp(OperationContext* opCtx);

}  // namespace mongo::query_settings::query_settings_details
