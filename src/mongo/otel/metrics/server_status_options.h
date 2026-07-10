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
