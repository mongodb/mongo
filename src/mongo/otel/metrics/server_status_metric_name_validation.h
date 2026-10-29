// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo::otel::metrics {

/**
 * Validates `dottedPath` for `ServerStatusOptions` (nested under `serverStatus.metrics`).
 *
 * Rules:
 * - non-empty ASCII
 * - no maximum length (shape rules only)
 * - no leading or trailing dot
 * - no empty segments
 * - each segment starts with `a–z`. Remaining characters may be `a–z`, `A–Z`, or `0–9`. No `_` or
 *   `-`.
 * - must not equal "metrics" or start with "metrics" since the prefix is added automatically.
 */
Status validateServerStatusMetricPath(std::string_view dottedPath);

}  // namespace mongo::otel::metrics
