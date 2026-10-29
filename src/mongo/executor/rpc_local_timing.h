// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/rpc_local_timing.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace executor {

/**
 * Which outbound RPC segment an RpcLocalTiming measures. Recorded as the metric's "kind"
 * attribute: "initial" for a command's first (or only) RPC, "exhaust_continuation" for each
 * subsequent reply read from an exhaust stream, and "fire_and_forget" for requests that do not
 * wait for a response.
 */
enum class RpcLocalTimingKind { kInitial, kExhaustContinuation, kFireAndForget };

/** Selects the client path used for an outbound RPC segment. */
enum class RpcLocalTimingMode { kAsync, kSync };

/**
 * Records one sample for a finished RPC segment, if the timing object has not already been
 * finished. A repeated call records nothing.
 *
 * Async timing objects are recorded from continuations on their own future chain or from an
 * unwinding path in the function that created them. As a rule to simplify reasoning about timings,
 * do not record from timeout or cancellation callbacks, or from destructors of objects whose
 * lifetime crosses an asynchronous boundary.
 */
void recordRpcLocalLatency(RpcLocalTiming& timing,
                           RpcLocalTimingKind kind,
                           RpcLocalTimingMode mode);

inline constexpr std::string_view toString(RpcLocalTimingKind kind) {
    switch (kind) {
        case RpcLocalTimingKind::kInitial:
            return "initial";
        case RpcLocalTimingKind::kExhaustContinuation:
            return "exhaust_continuation";
        case RpcLocalTimingKind::kFireAndForget:
            return "fire_and_forget";
    }

    MONGO_UNREACHABLE;
}

inline constexpr std::string_view toString(RpcLocalTimingMode mode) {
    switch (mode) {
        case RpcLocalTimingMode::kAsync:
            return "async";
        case RpcLocalTimingMode::kSync:
            return "sync";
    }

    MONGO_UNREACHABLE;
}

}  // namespace executor
}  // namespace mongo
