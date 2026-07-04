/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
