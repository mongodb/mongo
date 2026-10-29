// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * EXPAND table of QueryKnob<T>s mirroring the server parameters in query_integration_knobs.idl.
 *
 * TODO(SERVER-134995): register internalQueryStatsWriteCmdSampleRate once write commands are
 * query-settings-eligible.
 */

#pragma once

#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/util/modules.h"

// clang-format off
#define MONGO_EXPAND_QUERY_KNOBS_INTEGRATION(KNOB)                \
    KNOB(kQueryStatsSampleRate,                                    \
         kInternalQueryStatsSampleRateName,                        \
         internalQueryStatsSampleRate,                             \
         getQueryStatsSampleRate)                                  \
    /* End MONGO_EXPAND_QUERY_KNOBS_INTEGRATION */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryIntegrationKnobs, MONGO_EXPAND_QUERY_KNOBS_INTEGRATION)
}  // namespace mongo::query_knobs
