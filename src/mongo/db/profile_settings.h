/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/database_name.h"
#include "mongo/db/profile_filter.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/unordered_map.h"

#include <memory>


namespace mongo {

class ServiceContext;

struct ProfileSettings {
    int level;
    std::shared_ptr<const ProfileFilter> filter;  // nullable

    ProfileSettings(int level, std::shared_ptr<ProfileFilter> filter)
        : level(level), filter(filter) {
        // ProfileSettings represents a state, not a request to change the state.
        // -1 is not a valid profiling level: it is only used in requests, to represent
        // leaving the state unchanged.
        invariant(0 <= level && level <= 2, str::stream() << "Invalid profiling level: " << level);
    }

    ProfileSettings() = default;

    bool operator==(const ProfileSettings& other) const {
        return level == other.level && filter == other.filter;
    }
};

/**
 * DatabaseProfileSettings holds the default and database-specific profiling settings for the query
 * profiler.
 *
 * All functions that modify the profile settings are assumed to be called very infrequently, and
 * thus enable performance optimizations for calls to read-only functions.
 */
class DatabaseProfileSettings {
public:
    static DatabaseProfileSettings& get(ServiceContext* svcCtx);

    DatabaseProfileSettings();

    /**
     * Update the global default profile level.
     */
    void setDefaultLevel(int level);

    /**
     * Set the global 'ProfileFilter' default.
     */
    void setDefaultFilter(std::shared_ptr<ProfileFilter> filter);

    /**
     * Return the global 'ProfileFilter' default.
     */
    std::shared_ptr<ProfileFilter> getDefaultFilter() const;

    /**
     * Updates the profile filter on all databases with non-default settings, and changes the
     * default filter.
     */
    void setAllDatabaseProfileFiltersAndDefault(std::shared_ptr<ProfileFilter> filter);

    /**
     * Sets 'newProfileSettings' as the profiling settings for the database 'dbName'.
     */
    void setDatabaseProfileSettings(const DatabaseName& dbName, ProfileSettings newProfileSettings);

    /**
     * Fetches the profiling settings for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     */
    ProfileSettings getDatabaseProfileSettings(const DatabaseName& dbName) const;

    /**
     * Fetches the profiling level for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     *
     * There is no corresponding setDatabaseProfileLevel; use setDatabaseProfileSettings instead.
     * This method only exists as a convenience.
     */
    int getDatabaseProfileLevel(const DatabaseName& dbName) const {
        return getDatabaseProfileSettings(dbName).level;
    }

    /**
     * Clears the database profile settings entry for 'dbName'.
     */
    void clearDatabaseProfileSettings(const DatabaseName& dbName);

private:
    // Assume that database operators are not changing profiler settings for any
    // performance-sensitive paths. Protects all below members.
    mutable RWMutex _mutex;

    // Contains non-default database profile settings. New collections, current collections and
    // views must all be able to access the correct profile settings for the database in which they
    // reside. Simple database name to struct ProfileSettings map.
    using DatabaseProfileSettingsMap = stdx::unordered_map<DatabaseName, ProfileSettings>;
    DatabaseProfileSettingsMap _databaseProfileSettings;

    // The following hold the default profiling settings to use when none are explicitly set
    // on a database.
    std::shared_ptr<ProfileFilter> _defaultProfileFilter;
    int _defaultLevel = 0;
};


}  // namespace mongo
