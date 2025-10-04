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

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mongo {
class StringData;
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
    virtual std::string getTableCreateConfig(StringData tableName);
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
    std::string getTableCreateConfig(StringData tableName) const;

private:
    std::vector<std::unique_ptr<WiredTigerCustomizationHooks>> _hooks;
};

}  // namespace mongo
