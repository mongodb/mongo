// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <vector>

namespace mongo {
namespace process_health {

using HealthObserverFactory [[MONGO_MOD_PUBLIC]] =
    std::function<std::unique_ptr<HealthObserver>(ServiceContext*)>;

/**
 * Registration mechanism for all health observers.
 * This is static class not requiring an instance to work.
 */
class [[MONGO_MOD_PUBLIC]] HealthObserverRegistration {
public:
    /**
     * Registers a factory method, which will be invoked later to instantiate the observer.
     * This must be invoked by static initializers, the code is not internally synchronized.
     *
     * factoryCallback creates observer instance when invoked.
     */
    static void registerObserverFactory(HealthObserverFactory factory);

    /**
     * Invokes all registered factories and returns new instances.
     * The ownership of all observers is transferred to the invoker.
     */
    static std::vector<std::unique_ptr<HealthObserver>> instantiateAllObservers(
        ServiceContext* svcCtx);

    /** Swaps out the list of registered factories. */
    static std::vector<HealthObserverFactory> setObserverFactories_ForTest(
        std::vector<HealthObserverFactory> factories);
};

}  // namespace process_health
}  // namespace mongo
