/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/scripting/mozjs/shell/internal_module_registry.h"

#include "mongo/util/concurrency/with_lock.h"

#include <mutex>
#include <string>
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
