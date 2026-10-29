// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"

namespace mongo {
struct HttpClientOptions {
    /**
     * Boolean flag that indicates whether verbose logging for http clients should be enabled.
     *
     * Note: only affects new handles. This means that if connection pooling is in use, this will
     * not take affect on existing connections.
     */
    Atomic<bool> verboseLogging;
};

extern HttpClientOptions httpClientOptions;
}  // namespace mongo
