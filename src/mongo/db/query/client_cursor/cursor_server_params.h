// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

int getClientCursorMonitorFrequencySecs();

// Period of time after which mortal cursors are killed for inactivity. Configurable with server
// parameter "cursorTimeoutMillis".
long long getCursorTimeoutMillis();

Milliseconds getDefaultCursorTimeoutMillis();

}  // namespace mongo
