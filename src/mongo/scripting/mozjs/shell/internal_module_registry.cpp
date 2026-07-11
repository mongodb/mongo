// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/internal_module_registry.h"

#include "mongo/util/concurrency/with_lock.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mongo::mozjs {
namespace {

using InternalModuleMap = std::unordered_map<std::string, InternalModuleRegistration>;

InternalModuleMap& getInternalModuleMap(WithLock) {
    static InternalModuleMap moduleMap;
    return moduleMap;
}

std::mutex& getInternalModuleMapMutex() {
    static std::mutex moduleMapMutex;
    return moduleMapMutex;
}

void addInternalModuleRegistration(std::string_view moduleName,
                                   InternalModuleInitializer initialize,
                                   const ::mongo::JSFile* setupFile) {
    if (moduleName.empty() || initialize == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(getInternalModuleMapMutex());
    getInternalModuleMap(lock)[std::string(moduleName)] =
        InternalModuleRegistration{std::string(moduleName), initialize, setupFile};
}

}  // namespace

std::vector<InternalModuleRegistration> listRegisteredInternalModules() {
    std::vector<InternalModuleRegistration> registrations;

    std::lock_guard<std::mutex> lock(getInternalModuleMapMutex());
    const auto& moduleMap = getInternalModuleMap(lock);
    registrations.reserve(moduleMap.size());
    for (const auto& [_, registration] : moduleMap) {
        registrations.push_back(registration);
    }

    return registrations;
}

InternalModuleRegistrar::InternalModuleRegistrar(std::string_view moduleName,
                                                 InternalModuleInitializer initialize,
                                                 const ::mongo::JSFile* setupFile) {
    addInternalModuleRegistration(moduleName, initialize, setupFile);
}

}  // namespace mongo::mozjs
