// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/shard_role/lock_manager/fill_locker_info.h"

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/lock_stats.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

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

        static const auto resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, DatabaseName::kLocal);
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
