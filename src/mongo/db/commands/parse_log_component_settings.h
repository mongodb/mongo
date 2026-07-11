// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_component.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

class BSONObj;
template <typename T>
class StatusWith;

/**
 * One parsed LogComponent and desired log level
 */
struct [[MONGO_MOD_PUBLIC]] LogComponentSetting {
    LogComponentSetting(logv2::LogComponent c, int lvl) : component(c), level(lvl) {}

    logv2::LogComponent component;
    int level;
};

/**
 * Parses instructions for modifying component log levels from "settings".
 *
 * Returns an error status describing why parsing failed, or a vector of LogComponentSettings,
 * each describing how to change a particular log components verbosity level.
 */
[[MONGO_MOD_PUBLIC]] StatusWith<std::vector<LogComponentSetting>> parseLogComponentSettings(
    const BSONObj& settings);

}  // namespace mongo
