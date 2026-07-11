// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/profile_filter.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <memory>


namespace mongo {

class ServiceContext;

struct [[MONGO_MOD_PUBLIC]] ProfileSettings {
    int level{0};
    std::shared_ptr<const ProfileFilter> filter;  // nullable
    Milliseconds slowOpInProgressThreshold{0};

    ProfileSettings(int level,
                    std::shared_ptr<ProfileFilter> filter,
                    Milliseconds slowOpInProgressThreshold)
        : level(level),
          filter(std::move(filter)),
          slowOpInProgressThreshold(slowOpInProgressThreshold) {
        // ProfileSettings represents a state, not a request to change the state.
        // -1 is not a valid profiling level: it is only used in requests, to represent
        // leaving the state unchanged.
        invariant(0 <= level && level <= 2, str::stream() << "Invalid profiling level: " << level);
    }

    ProfileSettings() = default;

    bool operator==(const ProfileSettings& other) const = default;
};

/**
 * DatabaseProfileSettings holds the default and database-specific profiling settings for the query
 * profiler.
 *
 * All functions that modify the profile settings are assumed to be called very infrequently, and
 * thus enable performance optimizations for calls to read-only functions.
 */
class [[MONGO_MOD_PUBLIC]] DatabaseProfileSettings {
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
     * Set the global 'ProfileFilter' default.
     */
    void setDefaultSlowOpInProgressThreshold(Milliseconds slowOpInProgressThreshold);

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
    Milliseconds _defaultSlowOpInProgressThreshold{5000};
};


}  // namespace mongo
