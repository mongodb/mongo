// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/client_cursor/cursor_server_params.h"

#include "mongo/db/query/client_cursor/cursor_server_params_gen.h"
#include "mongo/platform/atomic.h"

namespace mongo {

int getClientCursorMonitorFrequencySecs() {
    return gClientCursorMonitorFrequencySecs.load();
}

long long getCursorTimeoutMillis() {
    return gCursorTimeoutMillis.load();
}

Milliseconds getDefaultCursorTimeoutMillis() {
    return Milliseconds(kCursorTimeoutMillisDefault);
}

}  // namespace mongo
