// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

struct ServerGlobalParams;

// Checks various startup conditions and logs any necessary warnings that
// are common to both mongod and mongos processes.
void logCommonStartupWarnings(const ServerGlobalParams& serverParams);
}  // namespace mongo
