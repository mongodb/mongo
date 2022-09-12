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


#include "mongo/db/stats/fill_locker_info.h"

#include <algorithm>

#include "mongo/db/concurrency/locker.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

void fillLockerInfo(const Locker::LockerInfo& lockerInfo, BSONObjBuilder& infoBuilder) {
    // "locks" section
    BSONObjBuilder locks(infoBuilder.subobjStart("locks"));
    const size_t locksSize = lockerInfo.locks.size();

    // Only add the last lock of each type, and use the largest mode encountered. Each type of
    // global resource is reported as its own type.
    constexpr auto totalResourceTypesCount =
        static_cast<uint8_t>(ResourceGlobalId::kNumIds) + ResourceTypesCount;
    LockMode modeForType[totalResourceTypesCount - 1] =
        {};  // default initialize to zero (min value)
    for (size_t i = 0; i < locksSize; i++) {
        const Locker::OneLock& lock = lockerInfo.locks[i];
        const ResourceType lockType = lock.resourceId.getType();

        auto index = lockType == RESOURCE_GLOBAL
            ? lock.resourceId.getHashId()
            : static_cast<uint8_t>(ResourceGlobalId::kNumIds) + lockType - 1;

        // Note: The value returned by getHashId() for a RESOURCE_GLOBAL lock is equivalent to the
        // lock's ResourceGlobalId enum value and should always be less than
        // ResourceGlobalId::kNumIds.
        // Enforce that the computed 'index' for the resource's type is be within the expected
        // bounds.
        invariant(
            index < totalResourceTypesCount,
            str::stream() << "Invalid index used to fill locker statistics -  index:  " << index
                          << ", totalResourceTypesCount: " << totalResourceTypesCount
                          << ", (lockType == RESOURCE_GLOBAL): " << (lockType == RESOURCE_GLOBAL));

        const LockMode lockMode = std::max(lock.mode, modeForType[index]);

        // Check that lockerInfo is sorted on resource type
        invariant(i == 0 || lockType >= lockerInfo.locks[i - 1].resourceId.getType());

        if (lock.resourceId == resourceIdLocalDB) {
            locks.append("local", legacyModeName(lock.mode));
            continue;
        }

        modeForType[index] = lockMode;

        if (i + 1 < locksSize && lockerInfo.locks[i + 1].resourceId.getType() == lockType &&
            (lockType != RESOURCE_GLOBAL ||
             lock.resourceId.getHashId() == lockerInfo.locks[i + 1].resourceId.getHashId())) {
            continue;  // skip this lock as it is not the last one of its type
        } else if (lockType == RESOURCE_GLOBAL) {
            locks.append(
                resourceGlobalIdName(static_cast<ResourceGlobalId>(lock.resourceId.getHashId())),
                legacyModeName(lockMode));
        } else {
            locks.append(resourceTypeName(lockType), legacyModeName(lockMode));
        }
    }
    locks.done();

    // "waitingForLock" section
    infoBuilder.append("waitingForLock", lockerInfo.waitingResource.isValid());

    // "lockStats" section
    {
        BSONObjBuilder lockStats(infoBuilder.subobjStart("lockStats"));
        lockerInfo.stats.report(&lockStats);
        lockStats.done();
    }
}

}  // namespace mongo
