// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
class ServiceContext;

// Interface and default implementation for WiredTiger customization hooks
class WiredTigerCustomizationHooks {
public:
    static void set(ServiceContext* service,
                    std::unique_ptr<WiredTigerCustomizationHooks> customHooks);

    static WiredTigerCustomizationHooks* get(ServiceContext* service);

    virtual ~WiredTigerCustomizationHooks();

    /**
     * Returns true if the customization hooks are enabled.
     */
    virtual bool enabled() const;

    /**
     *  Gets an additional configuration string for the provided table name on a
     *  `WT_SESSION::create` call.
     */
    virtual std::string getTableCreateConfig(std::string_view tableName);
};

/**
 * Registry to store multiple WiredTiger customization hooks.
 */
class WiredTigerCustomizationHooksRegistry {
public:
    static WiredTigerCustomizationHooksRegistry& get(ServiceContext* serviceContext);

    /**
     * Adds a WiredTiger customization hook to the registry. Multiple hooks can be
     * added, and their configurations will be combined.
     */
    void addHook(std::unique_ptr<WiredTigerCustomizationHooks> custHook);

    /**
     * Gets a combined configuration string from all hooks in the registry for
     * the provided table name during the `WT_SESSION::create` call.
     */
    std::string getTableCreateConfig(std::string_view tableName) const;

private:
    std::vector<std::unique_ptr<WiredTigerCustomizationHooks>> _hooks;
};

}  // namespace mongo
