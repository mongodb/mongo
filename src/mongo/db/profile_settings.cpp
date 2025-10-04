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

    return {_defaultLevel, _defaultProfileFilter};
}

void DatabaseProfileSettings::clearDatabaseProfileSettings(const DatabaseName& dbName) {
    std::unique_lock lk(_mutex);
    _databaseProfileSettings.erase(dbName);
}

}  // namespace mongo
