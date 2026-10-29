// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

namespace mongo::repl {

/**
 * Observer interface for opTime advances. Implementations must be fast (a single atomic store is
 * ideal).
 *
 * Callbacks are invoked from a dedicated dispatcher thread outside of any replication lock.
 * Observers are registered once before startup and are never removed.
 */
class [[MONGO_MOD_OPEN]] OpTimeObserver {
public:
    virtual ~OpTimeObserver() = default;

    /**
     * Called whenever the observed opTime advances to a new non-null value.
     *
     * NOTE: Intermediate values may be skipped. If the opTime advances multiple times before this
     * observer can be notified, only the most recent value will be delivered. Implementations must
     * not assume they will observe every opTime. The only guarantee is that this method will
     * eventually be called with the most recently applied opTime.
     */
    virtual void onOpTime(const Timestamp& ts) = 0;
};

}  // namespace mongo::repl
