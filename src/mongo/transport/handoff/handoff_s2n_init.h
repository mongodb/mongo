// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"

namespace mongo {

/**
 * Initializes the s2n-tls library via `s2n_init()`, but only if `s2n_init()` has not been called
 * previously.
 * Returns `Status::OK()` on success, or on failure returns an `InternalError` with a message
 * describing the failure.
 * `s2nInitOnce` must be called from the process's main thread.
 */
Status s2nInitOnce();

}  // namespace mongo
