/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

ServiceContext::ConstructorActionRegisterer setWiredTigerCustomizationHooks{
    "SetWiredTigerCustomizationHooks", [](ServiceContext* service) {
        auto customizationHooks = std::make_unique<WiredTigerCustomizationHooks>();
        WiredTigerCustomizationHooks::set(service, std::move(customizationHooks));
    }};

const auto getCustomizationHooks =
    ServiceContext::declareDecoration<std::unique_ptr<WiredTigerCustomizationHooks>>();

const auto getWiredTigerCustomizationHooksRegistry =
    ServiceContext::declareDecoration<WiredTigerCustomizationHooksRegistry>();

}  // namespace


WiredTigerCustomizationHooksRegistry& WiredTigerCustomizationHooksRegistry::get(
    ServiceContext* service) {
    return getWiredTigerCustomizationHooksRegistry(service);
}


void WiredTigerCustomizationHooksRegistry::addHook(
    std::unique_ptr<WiredTigerCustomizationHooks> custHook) {
    invariant(custHook);
    _hooks.push_back(std::move(custHook));
}

std::string WiredTigerCustomizationHooksRegistry::getTableCreateConfig(StringData tableName) const {
    str::stream config;
    for (const auto& h : _hooks) {
        config << h->getTableCreateConfig(tableName);
    }
    return config;
}

void WiredTigerCustomizationHooks::set(ServiceContext* service,
                                       std::unique_ptr<WiredTigerCustomizationHooks> customHooks) {
    auto& hooks = getCustomizationHooks(service);
    invariant(customHooks);
    hooks = std::move(customHooks);
}

WiredTigerCustomizationHooks* WiredTigerCustomizationHooks::get(ServiceContext* service) {
    return getCustomizationHooks(service).get();
}

WiredTigerCustomizationHooks::~WiredTigerCustomizationHooks() {}

bool WiredTigerCustomizationHooks::enabled() const {
    return false;
}

std::string WiredTigerCustomizationHooks::getTableCreateConfig(StringData tableName) {
    return "";
}

}  // namespace mongo
