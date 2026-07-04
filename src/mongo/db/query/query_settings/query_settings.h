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
#include "mongo/db/query/query_knobs/query_knob_snapshot.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/util/modules.h"

namespace mongo::query_settings {

/**
 * Returns the resolved QuerySettings for 'opCtx'. Returns empty settings for operations that are
 * not query-settings-eligible ('NotStarted') or for which resolution matched nothing ('Empty').
 * Must not be called while resolution is still pending ('Pending'): the operation is eligible but
 * settings have not been installed yet, so reading them would observe stale values and asserts
 * instead.
 */
MONGO_MOD_PUBLIC const QuerySettings& forOp(OperationContext* opCtx);

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
MONGO_MOD_PUBLIC KnobOverrideResult tryOverrideQueryKnobValues(OperationContext* opCtx,
                                                               QueryKnobSnapshot& snapshot);

}  // namespace mongo::query_settings
