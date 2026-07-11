// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs/query_knob_snapshot.h"
#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/util/modules.h"

#include <variant>

namespace mongo::query_settings {

/**
 * Returns the resolved QuerySettings for 'opCtx'. Returns empty settings for operations that are
 * not query-settings-eligible ('NotStarted') or for which resolution matched nothing ('Empty').
 * Must not be called while resolution is still pending ('Pending'): the operation is eligible but
 * settings have not been installed yet, so reading them would observe stale values and asserts
 * instead.
 */
[[MONGO_MOD_PUBLIC]] const QuerySettings& forOp(OperationContext* opCtx);

/**
 * Outcome of 'tryOverrideQueryKnobValues'. Distinguishes the two "retry later" phases because they
 * imply different knob-read restrictions:
 *
 * kApplied    - Overrides (if any) are installed; 'snapshot' is final and all knob reads are
 *               allowed.
 * kNotStarted - No query-settings-eligible command has begun on the operation; all knob reads are
 *               allowed, but the caller must retry later.
 * kPending    - An eligible command has begun but settings have not been resolved yet; reading a
 *               PQS-settable knob now would observe a value the settings may be about to override,
 *               so such reads are forbidden until a retry returns 'kApplied'.
 */
enum class KnobOverrideResult { kApplied, kNotStarted, kPending };

/**
 * Applies any query-settings knob overrides for 'opCtx' onto 'snapshot' in place. Returns
 * 'kApplied' once the overrides have been resolved and applied (or there are none to apply, i.e.
 * the operation is not query-settings-eligible), meaning 'snapshot' is final. Otherwise leaves
 * 'snapshot' unchanged and returns 'kNotStarted' or 'kPending' (see 'KnobOverrideResult'); the
 * caller must retry later.
 */
[[MONGO_MOD_PUBLIC]] KnobOverrideResult tryOverrideQueryKnobValues(OperationContext* opCtx,
                                                                   QueryKnobSnapshot& snapshot);

/**
 * Attaches the operation's resolved query settings to 'request' via 'request.setQuerySettings'. A
 * no-op unless resolution produced non-default settings: only the 'QuerySettings' alternative
 * carries them (default resolutions collapse to 'Empty', and pending ones have not resolved yet).
 */
template <typename Request>
void addQuerySettingsToRequest(OperationContext* opCtx, Request& request) {
    auto& state = query_settings_details::getQuerySettingsStateForOp(opCtx);
    if (auto* settings = std::get_if<QuerySettings>(&state)) {
        request.setQuerySettings(*settings);
    }
}

/**
 * Adds the operation's resolved query settings to the slow query log 'attrs' under "querySettings".
 * A no-op unless resolution installed non-default settings (only the 'QuerySettings' alternative
 * carries them, as in 'addQuerySettingsToRequest').
 *
 * Unlike 'forOp', this reads the lifecycle state directly and never asserts on 'Pending', so it is
 * safe to call from log formatting for operations that began dispatch (hooks set 'Pending') but
 * never completed settings resolution.
 */
[[MONGO_MOD_PUBLIC]] void addQuerySettingsToSlowLog(OperationContext* opCtx,
                                                    logv2::DynamicAttributes& attrs);

}  // namespace mongo::query_settings
