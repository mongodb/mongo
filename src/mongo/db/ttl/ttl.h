// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Instantiates the TTLMonitor to periodically remove documents from TTL collections. Safe to call
 * again after shutdownTTLMonitor() has been called.
 */
void startTTLMonitor(ServiceContext* serviceContext, bool setupOnly = false);

/**
 * Shuts down the TTLMonitor if it is running. Safe to call multiple times.
 */
void shutdownTTLMonitor(ServiceContext* serviceContext);
}  // namespace mongo
