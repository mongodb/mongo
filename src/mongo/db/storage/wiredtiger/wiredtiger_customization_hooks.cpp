// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <memory>
#include <string_view>
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

std::string WiredTigerCustomizationHooksRegistry::getTableCreateConfig(
    std::string_view tableName) const {
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

std::string WiredTigerCustomizationHooks::getTableCreateConfig(std::string_view tableName) {
    return "";
}

}  // namespace mongo
