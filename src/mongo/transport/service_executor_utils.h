// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::transport {

Status launchServiceWorkerThread(unique_function<void()> task);

/**
 * Applies the client thread nice value to the calling thread if the current eligibility predicate
 * returns true; see registerClientThreadNiceEligibiliyPredicate() below.
 */
void applyClientThreadNiceValue();

/**
 * A predicate deciding whether the calling thread should currently have the nice value applied to
 * it. Called on every accepted connection, so it must be cheap (e.g. an atomic load) and must not
 * block.
 */
using ClientThreadNiceEligibilityPredicate = unique_function<bool()>;

/**
 * Registers the predicate consulted by `applyClientThreadNiceValue()`. Intended for a single
 * registration at startup by whichever component knows the criteria under which client threads
 * should be deprioritized (e.g. a storage or replication mode); this layer intentionally has no
 * knowledge of what that criteria is. Last writer wins; pass an empty predicate to unregister.
 */
[[MONGO_MOD_PUBLIC]] void registerClientThreadNiceEligibilityPredicate(
    ClientThreadNiceEligibilityPredicate predicate);

/**
 * Supplies the CFS nice value to apply to eligible client connection threads. Called on every
 * accepted connection alongside the eligibility predicate, so it must be cheap (e.g. an atomic
 * load) and must not block.
 */
using ClientThreadNiceValueProvider = unique_function<int32_t()>;

/**
 * Registers the provider consulted by `applyClientThreadNiceValue()`. Intended for a single
 * registration at startup. Last writer wins; pass an empty provider to unregister.
 */
[[MONGO_MOD_PUBLIC]] void registerClientThreadNiceValueProvider(
    ClientThreadNiceValueProvider provider);

/** Number of client connection threads that successfully applied a non-zero nice value. */
int64_t getClientThreadsRenicedCount();

/** Number of client connection threads whose `setpriority` call failed. */
int64_t getClientThreadReniceFailedCount();

/**
 * Sink for OpenTelemetry/serverStatus reporting of client thread nice value activity.
 * Called on every accepted connection alongside `applyClientThreadNiceValue()`, so implementations
 * must be cheap and must not block.
 */
struct [[MONGO_MOD_PUBLIC]] ClientThreadNiceMetricsSink {
    // Invoked with the currently configured nice value every time it is evaluated, including 0,
    // so a metrics consumer can report an explicit "not configured" state.
    unique_function<void(int32_t)> onNiceValueObserved;
    unique_function<void()> onThreadReniced;
    unique_function<void()> onThreadReniceFailed;
};

/**
 * Registers the sink consulted by `applyClientThreadNiceValue()`. Last writer wins; pass a
 * default-constructed sink to unregister.
 */
[[MONGO_MOD_PUBLIC]] void registerClientThreadNiceMetricsSink(ClientThreadNiceMetricsSink sink);

}  // namespace mongo::transport
