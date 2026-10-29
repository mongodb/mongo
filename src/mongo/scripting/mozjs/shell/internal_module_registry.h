// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <js/RootingAPI.h>

struct JSContext;

namespace mongo {
struct JSFile;
}

namespace mongo::mozjs {

using InternalModuleInitializer = bool (*)(JSContext* cx, JS::HandleObject target);

struct InternalModuleRegistration {
    std::string moduleName;
    InternalModuleInitializer initialize;
    const ::mongo::JSFile* setupFile;
};

std::vector<InternalModuleRegistration> listRegisteredInternalModules();

class [[MONGO_MOD_PUBLIC]] InternalModuleRegistrar {
public:
    InternalModuleRegistrar(std::string_view moduleName,
                            InternalModuleInitializer initialize,
                            const ::mongo::JSFile* setupFile = nullptr);
};

}  // namespace mongo::mozjs

#define MONGO_INTERNAL_MODULE_CONCAT_IMPL(X, Y) X##Y
#define MONGO_INTERNAL_MODULE_CONCAT(X, Y) MONGO_INTERNAL_MODULE_CONCAT_IMPL(X, Y)

#define MONGO_REGISTER_INTERNAL_MODULE(MODULE_NAME, INITIALIZE_FN)                 \
    namespace {                                                                    \
    const ::mongo::mozjs::InternalModuleRegistrar MONGO_INTERNAL_MODULE_CONCAT(    \
        kInternalModuleRegistrar_, __LINE__)(MODULE_NAME, INITIALIZE_FN, nullptr); \
    }  // namespace

#define MONGO_REGISTER_INTERNAL_MODULE_WITH_SETUP(MODULE_NAME, INITIALIZE_FN, SETUP_FILE) \
    namespace {                                                                           \
    const ::mongo::mozjs::InternalModuleRegistrar MONGO_INTERNAL_MODULE_CONCAT(           \
        kInternalModuleRegistrar_, __LINE__)(MODULE_NAME, INITIALIZE_FN, SETUP_FILE);     \
    }  // namespace
