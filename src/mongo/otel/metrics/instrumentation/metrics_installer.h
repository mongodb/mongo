// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

class ServiceContext;

/**
 * Installs OpenTelemetry metrics common to all server roles.
 */
[[MONGO_MOD_PUBLIC]] void installCommonOtelMetrics(ServiceContext* svcCtx);

/**
 * Installs all OpenTelemetry instrumentation metrics for mongod. Intended to be called once at
 * startup from mongod_main.
 */
[[MONGO_MOD_PUBLIC]] void installMongodOtelMetrics(ServiceContext* svcCtx);

/**
 * Installs OpenTelemetry instrumentation metrics for mongos. Intended to be called once at startup
 * from mongos_main.
 */
[[MONGO_MOD_PUBLIC]] void installMongosOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
