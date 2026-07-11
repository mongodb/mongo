// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

/**
 * Dumps the contents of all locks to the log.
 */
[[MONGO_MOD_PUBLIC]] void dumpLockManager();

}  // namespace mongo
