// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Allow components a way to tell the watchdog what to watch.
 */
void registerWatchdogPath(std::string_view path);

/**
 * Get list of registered watchdog paths.
 */
std::vector<std::string>& getWatchdogPaths();

}  // namespace mongo
