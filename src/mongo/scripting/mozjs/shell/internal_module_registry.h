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

class MONGO_MOD_PUB InternalModuleRegistrar {
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
