// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/timer.h"

#include <mutex>
#include <optional>
#include <utility>

namespace mongo {

/**
 * Tracks local time for one outbound RPC while excluding time spent waiting for a response.
 *
 * The timing state may be shared by the local phases of one RPC attempt. Calls to finish() are
 * idempotent: only the first call returns an elapsed duration.
 *
 * All methods are internally synchronized and may be called concurrently, and in particular, an
 * unwinding path in the function that created the timing object may run concurrently with a
 * continuation on its own future chain. See the rule on recordRpcLocalLatency in
 * executor/rpc_local_timing.h.
 *
 * Not copyable or movable.
 *
 * The simplest use case would be the following:
 *
 * RpcLocalTiming timing;
 * rpc->start();
 * timing.beginResponseWait();
 * rpc->waitForResponse();
 * timing.endResponseWait();
 * auto elapsed = timing.finish();
 */
class RpcLocalTiming {
public:
    RpcLocalTiming() = default;
    explicit RpcLocalTiming(TickSource* source) : _timer(source) {}

    /**
     * Indicates that this timer is recording time waiting for a RPC response. No-op if this doesn't
     * make sense in the current timer state (e.g. already waiting, timer finished).
     */
    void beginResponseWait() {
        std::lock_guard lock(_mutex);
        if (!_finished && _timer.isRunning()) {
            _timer.pause();
        }
    }

    /**
     * Indicates that this timer is recording time after receiving a RPC response. No-op if this
     * doesn't make sense in the current timer state (e.g. already stopped waiting, timer finished).
     */
    void endResponseWait() {
        std::lock_guard lock(_mutex);
        if (!_finished && !_timer.isRunning()) {
            _timer.unpause();
        }
    }

    /** Finish timing and return the duration on first call, or std::nullopt for a repeated call. */
    std::optional<Microseconds> finish() {
        std::lock_guard lock(_mutex);
        if (std::exchange(_finished, true)) {
            return std::nullopt;
        }
        return _timer.elapsed();
    }

private:
    std::mutex _mutex;
    Timer _timer;
    bool _finished = false;
};

}  // namespace mongo
