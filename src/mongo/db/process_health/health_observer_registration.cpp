// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/health_observer_registration.h"

#include "mongo/util/static_immortal.h"

#include <utility>

namespace mongo {
namespace process_health {

namespace {

/** Returns static vector of all registrations. */
std::vector<HealthObserverFactory>& getObserverFactories() {
    static StaticImmortal<std::vector<HealthObserverFactory>> obj;
    return *obj;
}

}  // namespace

void HealthObserverRegistration::registerObserverFactory(HealthObserverFactory factory) {
    getObserverFactories().push_back(std::move(factory));
}

std::vector<std::unique_ptr<HealthObserver>> HealthObserverRegistration::instantiateAllObservers(
    ServiceContext* svcCtx) {
    std::vector<std::unique_ptr<HealthObserver>> result;
    auto&& factories = getObserverFactories();
    for (auto& cb : factories) {
        result.push_back(cb(svcCtx));
    }
    return result;
}

std::vector<HealthObserverFactory> HealthObserverRegistration::setObserverFactories_ForTest(
    std::vector<HealthObserverFactory> factories) {
    return std::exchange(getObserverFactories(), std::move(factories));
}

}  // namespace process_health
}  // namespace mongo
