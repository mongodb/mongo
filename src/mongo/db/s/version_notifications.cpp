/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/version_notifications.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

ScopedVersionNotification VersionNotifications::createNotification(const NamespaceString& nss,
                                                                   const ChunkVersion& version) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    std::shared_ptr<stdx::condition_variable> condVar = std::make_shared<std::condition_variable>();
    VersionAndConditionVariable versionAndConditionVariable = std::make_pair(version, condVar);

    auto mapIt = _versionAndConditionVariablesMap.find(nss);
    VersionAndConditionVariableList::iterator versionAndConditionVariableIt;
    if (mapIt == _versionAndConditionVariablesMap.end()) {
        // Make a list for this nss with our version-notification pair and put it into the map.
        VersionAndConditionVariableList list;
        versionAndConditionVariableIt = list.insert(list.end(), versionAndConditionVariable);
        _versionAndConditionVariablesMap.insert(std::make_pair(nss, std::move(list)));
    } else {
        // A list already exists for this nss. Add our version-notification pair to it.
        versionAndConditionVariableIt =
            mapIt->second.insert(mapIt->second.end(), versionAndConditionVariable);
    }

    return ScopedVersionNotification(nss, versionAndConditionVariableIt, this);
}

void VersionNotifications::setAndNotifyIfNewEpochOrGTE(const NamespaceString& nss,
                                                       const ChunkVersion& version) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto mapIt = _versionAndConditionVariablesMap.find(nss);
    if (mapIt == _versionAndConditionVariablesMap.end()) {
        return;
    }

    for (auto& versionAndConditionVariable : mapIt->second) {
        if (version.epoch() != versionAndConditionVariable.first.epoch() ||
            version >= versionAndConditionVariable.first) {
            versionAndConditionVariable.first = version;
            versionAndConditionVariable.second->notify_all();
        }
    }
}

void VersionNotifications::removeNotification(
    const NamespaceString& nss,
    VersionAndConditionVariableList::iterator versionAndConditionVariableIt) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto mapIt = _versionAndConditionVariablesMap.find(nss);
    invariant(mapIt != _versionAndConditionVariablesMap.end());

    // Remove the notification from the list.
    mapIt->second.erase(versionAndConditionVariableIt);

    // Erase list if it is now empty.
    if (mapIt->second.empty()) {
        _versionAndConditionVariablesMap.erase(mapIt);
    }
}

ScopedVersionNotification::ScopedVersionNotification(
    const NamespaceString& nss,
    VersionAndConditionVariableList::iterator versionAndConditionVariableIt,
    VersionNotifications* versionNotifications)
    : _nss(nss),
      _versionAndConditionVariableIt(std::move(versionAndConditionVariableIt)),
      _versionNotifications(std::move(versionNotifications)) {}

ScopedVersionNotification::~ScopedVersionNotification() {
    _versionNotifications->removeNotification(_nss, _versionAndConditionVariableIt);
}

std::shared_ptr<stdx::condition_variable> ScopedVersionNotification::condVar() {
    return _versionAndConditionVariableIt->second;
}

const ChunkVersion& ScopedVersionNotification::version() {
    return _versionAndConditionVariableIt->first;
}

}  // namespace mongo
