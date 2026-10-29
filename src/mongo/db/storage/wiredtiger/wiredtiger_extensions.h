// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo {

class ServiceContext;

// NEEDS_REPLACEMENT: extentions are hard-coded config strings we shouldn't allow other modules to
// control/modify (prefer something higher level).
class [[MONGO_MOD_NEEDS_REPLACEMENT]] WiredTigerExtensions {
public:
    static WiredTigerExtensions& get(ServiceContext* service);

    /**
     * Returns the `extensions=[...]` piece for a `wiredtiger_open` call.
     */
    std::string getOpenExtensionsConfig() const;

    /**
     * Adds an item to the `wiredtiger_open` extensions list.
     */
    void addExtension(std::string_view extensionConfigStr);

private:
    std::vector<std::string> _wtExtensions;
};

class SpillWiredTigerExtensions {
public:
    static SpillWiredTigerExtensions& get(ServiceContext* service);

    /**
     * Returns the `extensions=[...]` piece for a `wiredtiger_open` call.
     */
    std::string getOpenExtensionsConfig() const;

    /**
     * Adds an item to the `wiredtiger_open` extensions list.
     */
    void addExtension(std::string_view extensionConfigStr);

private:
    std::vector<std::string> _wtExtensions;
};

}  // namespace mongo
