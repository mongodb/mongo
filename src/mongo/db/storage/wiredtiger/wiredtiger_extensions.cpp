// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <string_view>

namespace mongo {

namespace {
const auto getConfigHooks = ServiceContext::declareDecoration<WiredTigerExtensions>();
const auto getSpillConfigHooks = ServiceContext::declareDecoration<SpillWiredTigerExtensions>();
}  // namespace

WiredTigerExtensions& WiredTigerExtensions::get(ServiceContext* service) {
    return getConfigHooks(service);
}

SpillWiredTigerExtensions& SpillWiredTigerExtensions::get(ServiceContext* service) {
    return getSpillConfigHooks(service);
}

std::string WiredTigerExtensions::getOpenExtensionsConfig() const {
    if (_wtExtensions.size() == 0) {
        return "";
    }

    StringBuilder extensions;
    extensions << "extensions=[";
    for (const auto& ext : _wtExtensions) {
        extensions << ext << ",";
    }
    extensions << "],";

    return extensions.str();
}

void WiredTigerExtensions::addExtension(std::string_view extensionConfigStr) {
    _wtExtensions.emplace_back(std::string{extensionConfigStr});
}

std::string SpillWiredTigerExtensions::getOpenExtensionsConfig() const {
    if (_wtExtensions.size() == 0) {
        return "";
    }

    StringBuilder extensions;
    extensions << "extensions=[";
    for (const auto& ext : _wtExtensions) {
        extensions << ext << ",";
    }
    extensions << "],";

    return extensions.str();
}

void SpillWiredTigerExtensions::addExtension(std::string_view extensionConfigStr) {
    _wtExtensions.emplace_back(std::string{extensionConfigStr});
}

}  // namespace mongo
