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

#pragma once

#include <list>
#include <map>

#include "mongo/db/namespace_string.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

struct ChunkVersion;
class NamespaceString;
class ScopedVersionNotification;

using VersionAndConditionVariable =
    std::pair<ChunkVersion, std::shared_ptr<stdx::condition_variable>>;

using VersionAndConditionVariableList = std::list<VersionAndConditionVariable>;

/**
 * Registers (version, condition variable) pairs on a namespace. The version value can then be
 * updated and the condition variable signaled. Use this class if you must wait for a version to
 * change and then signal any waiters.
 *
 * Returns scoped objects (ScopeVersionNotification) that can access the pairs, and provides a
 * notify function to notify all condition variables for a particular namespace when a version GTE
 * has been set.
 */
class VersionNotifications {
public:
    /**
     * Registers a new (version, condition variable) pair and stores a reference to it in the
     * returned scoped object.
     */
    ScopedVersionNotification createNotification(const NamespaceString& nss,
                                                 const ChunkVersion& version);

    /**
     * Goes through all (version, condition variable) pairs that exist for 'nss' and, if 'version'
     * is GTE the version in the pair, updates the pair's version to 'version' and signals the
     * pair's condition variable.
     */
    void setAndNotifyIfNewEpochOrGTE(const NamespaceString& nss, const ChunkVersion& version);

private:
    /**
     * Deletes the (version, condition variable) pair referred to by
     * 'versionAndConditionVariableIt'.
     */
    void removeNotification(
        const NamespaceString& nss,
        VersionAndConditionVariableList::iterator versionAndConditionVariableIt);

    // Protects the class state below.
    stdx::mutex _mutex;

    // Stores all the (version, condition variable) pairs in lists by nss.
    std::map<NamespaceString, VersionAndConditionVariableList> _versionAndConditionVariablesMap;

    // Give ScopedVersionNotification access to the removeNotifications function to clean up pairs.
    friend class ScopedVersionNotification;
};

/**
 * Provides accessor methods to a (version, condition variable) pair registered on the
 * VersionNotifications object that created it. When it goes out of scope, handles unregistering the
 * (version, condition variable) pair from the VersionNotifications that created it.
 */
class ScopedVersionNotification {
public:
    ScopedVersionNotification(
        const NamespaceString& nss,
        VersionAndConditionVariableList::iterator versionAndConditionVariableIt,
        VersionNotifications* versionNotifications);

    ~ScopedVersionNotification();

    std::shared_ptr<stdx::condition_variable> condVar();

    const ChunkVersion& version();

private:
    // Identifies the list in '_versionNotifications' at which _versionAndConditionVariableIt
    // points.
    const NamespaceString _nss;

    // List iterator pointing to the (version, condition variable) pair.
    VersionAndConditionVariableList::iterator _versionAndConditionVariableIt;

    // Owner of the value pointed at by the above iterator.
    VersionNotifications* _versionNotifications;
};

}  // namespace mongo
