// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/health_observer_registration.h"

#include <utility>

namespace mongo {
namespace process_health {

namespace {

using HealthObserverFactoryCallback =
    std::function<std::unique_ptr<HealthObserver>(ServiceContext* svcCtx)>;

// Returns static vector of all registrations.
// No synchronization is required as all the factories are registered during
// static initialization.
std::vector<HealthObserverFactoryCallback>* getObserverFactories() {
    static std::vector<HealthObserverFactoryCallback>* factories =
        new std::vector<HealthObserverFactoryCallback>();
    return factories;
}

}  // namespace

void HealthObserverRegistration::registerObserverFactory(
    std::function<std::unique_ptr<HealthObserver>(ServiceContext* svcCtx)> factoryCallback) {
    getObserverFactories()->push_back(std::move(factoryCallback));
}

std::vector<std::unique_ptr<HealthObserver>> HealthObserverRegistration::instantiateAllObservers(
    ServiceContext* svcCtx) {
    std::vector<std::unique_ptr<HealthObserver>> result;
    auto factories = *getObserverFactories();
    for (auto& cb : factories) {
        result.push_back(cb(svcCtx));
    }
    return result;
}

void HealthObserverRegistration::resetObserverFactoriesForTest() {
    getObserverFactories()->clear();
}

}  // namespace process_health
}  // namespace mongo
