// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/modules.h"

#include <compare>
#include <string>

[[MONGO_MOD_PUBLIC]];

namespace mongo::otel::metrics {

/**
 * Used for making an OTel metric get reported under the serverStatus "metrics" section.
 */
struct ServerStatusOptions {
    std::string dottedPath;
    ClusterRole role;
    /**
     * Set to true only for backward compatibility with pre-existing metrics whose paths fail
     * `validateServerStatusMetricPath()` (e.g., segment casing rules). New metrics must not use
     * this flag.
     *
     * Please note that when it is set to true, `MetricsService` will not validate `dottedPath` at
     * all. Callers must ensure the path is structurally safe (non-empty, no leading/trailing '.',
     * no "metrics." prefix, etc.).
     */
    bool skipPathValidation = false;
};

inline bool operator==(const ServerStatusOptions& lhs, const ServerStatusOptions& rhs) {
    return (lhs.dottedPath == rhs.dottedPath) && lhs.role.hasExclusively(rhs.role) &&
        (lhs.skipPathValidation == rhs.skipPathValidation);
}

}  // namespace mongo::otel::metrics
