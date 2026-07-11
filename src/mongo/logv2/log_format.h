// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] logv2 {

enum class LogFormat { kDefault, kJson, kPlain };
enum class LogTimestampFormat { kISO8601UTC, kISO8601Local };

}  // namespace logv2
}  // namespace mongo
