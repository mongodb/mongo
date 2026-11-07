// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo::otel::traces {

/**
 * Returns whether externally-initiated traces may be continued, i.e. the emergency off switch
 * `openTelemetryExternalTracing.enabled` has not been set to false.
 */
[[MONGO_MOD_PUBLIC]] bool isExternalTracingEnabled();

}  // namespace mongo::otel::traces
