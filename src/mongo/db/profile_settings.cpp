// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/profile_settings.h"

#include "mongo/db/service_context.h"

#include <memory>
#include <shared_mutex>

namespace mongo {
namespace {
const ServiceContext::Decoration<DatabaseProfileSettings> getDbProfileSettings =
    ServiceContext::declareDecoration<DatabaseProfileSettings>();
}

DatabaseProfileSettings& DatabaseProfileSettings::get(ServiceContext* svcCtx) {
    return getDbProfileSettings(svcCtx);
}

DatabaseProfileSettings::DatabaseProfileSettings() {}

void DatabaseProfileSettings::setDefaultFilter(std::shared_ptr<ProfileFilter> filter) {
    std::unique_lock lk(_mutex);
    _defaultProfileFilter = std::move(filter);
}

std::shared_ptr<ProfileFilter> DatabaseProfileSettings::getDefaultFilter() const {
    std::shared_lock lk(_mutex);
    return _defaultProfileFilter;
}

void DatabaseProfileSettings::setDefaultLevel(int level) {
    std::unique_lock lk(_mutex);
    _defaultLevel = level;
}

void DatabaseProfileSettings::setDefaultSlowOpInProgressThreshold(
    Milliseconds slowOpInProgressThreshold) {
    std::unique_lock lk(_mutex);
    _defaultSlowOpInProgressThreshold = slowOpInProgressThreshold;
}

void DatabaseProfileSettings::setAllDatabaseProfileFiltersAndDefault(
    std::shared_ptr<ProfileFilter> filter) {
    std::unique_lock lk(_mutex);
    for (auto it = _databaseProfileSettings.begin(); it != _databaseProfileSettings.end(); it++) {
        ProfileSettings clone = it->second;
        clone.filter = filter;
        it->second = std::move(clone);
    }
    _defaultProfileFilter = filter;
}

void DatabaseProfileSettings::setDatabaseProfileSettings(const DatabaseName& dbName,
                                                         ProfileSettings newProfileSettings) {
    std::unique_lock lk(_mutex);
    _databaseProfileSettings[dbName] = std::move(newProfileSettings);
}

ProfileSettings DatabaseProfileSettings::getDatabaseProfileSettings(
    const DatabaseName& dbName) const {
    std::shared_lock lk(_mutex);
    auto it = _databaseProfileSettings.find(dbName);
    if (it != _databaseProfileSettings.end()) {
        return it->second;
    }

    return {_defaultLevel, _defaultProfileFilter, _defaultSlowOpInProgressThreshold};
}

void DatabaseProfileSettings::clearDatabaseProfileSettings(const DatabaseName& dbName) {
    std::unique_lock lk(_mutex);
    _databaseProfileSettings.erase(dbName);
}

}  // namespace mongo
