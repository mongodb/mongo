// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/modules.h"

#include <functional>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class ServiceContext;

/**
 * Registers the specified initializer function `init` as the initialization handler for
 * TenantToShardCache enterprise modules.
 *
 * NOTE: This function may only be called once.
 * NOTE: This function is not multithread safe.
 */
void registerTenantToShardCacheInitializer(std::function<void(ServiceContext* service)> init);

/**
 * Performs initialization for TenantToShardCache enterprise modules, if present, otherwise does
 * nothing.
 *
 * This will call the function registered by `registerTenantToShardCacheInitializer`. It is safe
 * to call when no function has been registered.
 */
void initializeTenantToShardCache(ServiceContext* service);
}  // namespace mongo
