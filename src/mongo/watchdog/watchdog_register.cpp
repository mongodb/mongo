// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/watchdog/watchdog_register.h"

#include <string_view>

namespace mongo {

namespace {

std::vector<std::string> watchdogPaths;

}  // namespace

void registerWatchdogPath(std::string_view path) {
    if (!path.empty()) {
        watchdogPaths.push_back(std::string{path});
    }
}

std::vector<std::string>& getWatchdogPaths() {
    return watchdogPaths;
}

}  // namespace mongo
