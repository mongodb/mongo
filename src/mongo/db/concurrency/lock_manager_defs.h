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

#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <limits>
#include <string>

namespace mongo {

class Locker;

struct LockHead;
struct PartitionedLockHead;

/**
 * LockMode compatibility matrix.
 *
 * This matrix answers the question, "Is a lock request with mode 'Requested Mode' compatible with
 * an existing lock held in mode 'Granted Mode'?"
 *
 * | Requested Mode |                      Granted Mode                     |
 * |----------------|:------------:|:-------:|:--------:|:------:|:--------:|
 * |                |  MODE_NONE   | MODE_IS |  MODE_IX | MODE_S |  MODE_X  |
 * | MODE_IS        |      +       |    +    |     +    |    +   |          |
 * | MODE_IX        |      +       |    +    |     +    |        |          |
 * | MODE_S         |      +       |    +    |          |    +   |          |
 * | MODE_X         |      +       |         |          |        |          |
 */
enum LockMode : uint8_t {
    /** None */
    MODE_NONE = 0,
    /** Intent shared */
    MODE_IS = 1,
    /** Intent exclusive */
    MODE_IX = 2,
    /** Shared */
    MODE_S = 3,
    /** Exclusive */
    MODE_X = 4,

    /**
     * Counts the lock modes. Used for array size allocations, etc. Always insert new lock modes
     * above this entry.
     */
    LockModesCount
};

/**
 * Returns a human-readable name for the specified lock mode.
 */
const char* modeName(LockMode mode);

/**
 * Legacy lock mode names in parity for 2.6 reports.
 */
const char* legacyModeName(LockMode mode);

/**
 * Mode A is covered by mode B if the set of conflicts for mode A is a subset of the set of
 * conflicts for mode B. For example S is covered by X. IS is covered by S. However, IX is not
 * covered by S or IS.
 */
bool isModeCovered(LockMode mode, LockMode coveringMode);

/**
 * Returns whether the passed in mode is S or IS. Used for validation checks.
 */
constexpr bool isSharedLockMode(LockMode mode) {
    return (mode == MODE_IS || mode == MODE_S);
}


/**
 * Return values for the locking functions of the lock manager.
 */
enum LockResult {

    /**
     * The lock request was granted and is now on the granted list for the specified resource.
     */
    LOCK_OK,

    /**
     * The lock request was not granted because of conflict. If this value is returned, the
     * request was placed on the conflict queue of the specified resource and a call to the
     * LockGrantNotification::notify callback should be expected with the resource whose lock
     * was requested.
     */
    LOCK_WAITING,

    /**
     * The lock request waited, but timed out before it could be granted. This value is never
     * returned by the LockManager methods here, but by the Locker class, which offers
     * capability to block while waiting for locks.
     */
    LOCK_TIMEOUT,

    /**
     * This is used as an initializer value. Should never be returned.
     */
    LOCK_INVALID
};


/**
 * Hierarchy of resource types. The lock manager knows nothing about this hierarchy, it is
 * purely logical. Resources of different types will never conflict with each other.
 *
 * While the lock manager does not know or care about ordering, the general policy is that
 * resources are acquired in the order below. For example, one might first acquire a
 * RESOURCE_GLOBAL and then the desired RESOURCE_DATABASE, both using intent modes, and
 * finally a RESOURCE_COLLECTION in exclusive mode. When locking multiple resources of the
 * same type, the canonical order is by resourceId order.
 *
 * It is OK to lock resources out of order, but it is the users responsibility to ensure
 * ordering is consistent so deadlock cannot occur.
 */
enum ResourceType {
    RESOURCE_INVALID = 0,

    /**  Used for global exclusive operations */
    RESOURCE_GLOBAL,

    /** Encompasses resources belonging to a tenant, if in multi-tenant mode.*/
    RESOURCE_TENANT,

    /** Generic resources, used for multi-granularity locking, together with the above locks */
    RESOURCE_DATABASE,
    RESOURCE_COLLECTION,
    RESOURCE_METADATA,

    /**
     * Resource DDL types used for multi-granularity locking on DDL operations.
     * These resources are not related to the storage hierarchy.
     */
    RESOURCE_DDL_DATABASE,
    RESOURCE_DDL_COLLECTION,

    /**
     * Resource type used for locking general resources not related to the storage hierarchy. These
     * can't be created manually, use Lock::ResourceMutex::ResourceMutex() instead.
     */
    RESOURCE_MUTEX,

    /** Counts the rest. Always insert new resource types above this entry. */
    ResourceTypesCount
};

/**
 * IDs for usages of RESOURCE_GLOBAL.
 */
enum class ResourceGlobalId : uint8_t {
    kMultiDocumentTransactionsBarrier,
    kReplicationStateTransitionLock,
    kGlobal,

    // The number of global resource ids. Always insert new ids above this entry.
    kNumIds
};

/**
 * Maps the resource id to a human-readable string.
 */
inline constexpr const char* ResourceTypeNames[] = {"Invalid",
                                                    "Global",
                                                    "Tenant",
                                                    "Database",
                                                    "Collection",
                                                    "Metadata",
                                                    "DDLDatabase",
                                                    "DDLCollection",
                                                    "Mutex"};

/**
 * Maps the global resource id to a human-readable string.
 */
inline constexpr const char* ResourceGlobalIdNames[] = {
    "MultiDocumentTransactionsBarrier",
    "ReplicationStateTransition",
    "Global",
};

// Ensure we do not add new types without updating the names array.
MONGO_STATIC_ASSERT((sizeof(ResourceTypeNames) / sizeof(ResourceTypeNames[0])) ==
                    ResourceTypesCount);

// Ensure we do not add new global resource ids without updating the names array.
MONGO_STATIC_ASSERT((sizeof(ResourceGlobalIdNames) / sizeof(ResourceGlobalIdNames[0])) ==
                    static_cast<uint8_t>(ResourceGlobalId::kNumIds));

/**
 * Returns a human-readable name for the specified resource type.
 */
constexpr const char* resourceTypeName(ResourceType resourceType) {
    return ResourceTypeNames[resourceType];
}

/**
 * Returns a human-readable name for the specified global resource.
 */
constexpr const char* resourceGlobalIdName(ResourceGlobalId id) {
    return ResourceGlobalIdNames[static_cast<uint8_t>(id)];
}

/**
 * Uniquely identifies a lockable resource.
 */
class ResourceId {
public:
    // We only use 4 bits for the resource type in the ResourceId hash
    static constexpr size_t resourceTypeBits = 4;
    MONGO_STATIC_ASSERT(ResourceTypesCount <= (1 << resourceTypeBits));

    ResourceId(ResourceType type, const NamespaceString& nss)
        : _fullHash(fullHash(type, hashStringData(nss.toStringForResourceId()))) {
        verifyNoResourceMutex(type);
    }
    ResourceId(ResourceType type, const DatabaseName& dbName)
        : _fullHash(fullHash(type, hashStringData(dbName.toStringForResourceId()))) {
        verifyNoResourceMutex(type);
    }
    ResourceId(ResourceType type, uint64_t hashId) : _fullHash(fullHash(type, hashId)) {
        verifyNoResourceMutex(type);
    }
    ResourceId(ResourceType type, const TenantId& tenantId)
        : _fullHash{fullHash(type, hashStringData(tenantId.toString()))} {
        verifyNoResourceMutex(type);
    }
    constexpr ResourceId() : _fullHash(0) {}

    bool isValid() const {
        return getType() != RESOURCE_INVALID;
    }

    operator uint64_t() const {
        return _fullHash;
    }

    // This defines the canonical locking order, first by type and then hash id
    bool operator<(const ResourceId& rhs) const {
        return _fullHash < rhs._fullHash;
    }

    ResourceType getType() const {
        return static_cast<ResourceType>(_fullHash >> (64 - resourceTypeBits));
    }

    uint64_t getHashId() const {
        return _fullHash & (std::numeric_limits<uint64_t>::max() >> resourceTypeBits);
    }

    std::string toString() const;

    template <typename H>
    friend H AbslHashValue(H h, const ResourceId& resource) {
        return H::combine(std::move(h), resource._fullHash);
    }

private:
    friend class ResourceCatalog;

    ResourceId(uint64_t fullHash) : _fullHash(fullHash) {}

    static void verifyNoResourceMutex(ResourceType type) {
        invariant(
            type != RESOURCE_MUTEX,
            "Can't create a ResourceMutex directly, use Lock::ResourceMutex::ResourceMutex().");
    }

    /**
     * The top 'resourceTypeBits' bits of '_fullHash' represent the resource type,
     * while the remaining bits contain the bottom bits of the hashId. This avoids false
     * conflicts between resources of different types, which is necessary to prevent deadlocks.
     */
    uint64_t _fullHash;

    static uint64_t fullHash(ResourceType type, uint64_t hashId) {
        return (static_cast<uint64_t>(type) << (64 - resourceTypeBits)) +
            (hashId & (std::numeric_limits<uint64_t>::max() >> resourceTypeBits));
    }

    static uint64_t hashStringData(StringData str) {
        return absl::hash_internal::CityHash64(str.data(), str.size());
    }
};

#ifndef MONGO_CONFIG_DEBUG_BUILD
// Treat the resource ids as 64-bit integers in release mode in order to ensure we do
// not spend too much time doing comparisons for hashing.
MONGO_STATIC_ASSERT(sizeof(ResourceId) == sizeof(uint64_t));
#endif

// Type to uniquely identify a given locker object
typedef uint64_t LockerId;

// Hardcoded resource id for the oplog collection, which is special-cased both for resource
// acquisition purposes and for statistics reporting.
extern const ResourceId resourceIdLocalDB;

// Hardcoded resource id for admin db. This is to ensure direct writes to auth collections
// are serialized (see SERVER-16092)
extern const ResourceId resourceIdAdminDB;

// Global lock. Every server operation, which uses the Locker must acquire this lock at least
// once. See comments in the header file (begin/endTransaction) for more information.
extern const ResourceId resourceIdGlobal;

// Hardcoded resource id for draining prepared transactions and avoiding a deadlock with global lock
// acquisitions in strong mode. This lock is acquired before the RSTL and resourceIdGlobal. It is
// acquired by operations processing transaction statements, and by operations acquiring the global
// lock in non-intent mode; all other requests skip this acquisition. It is acquired in the same
// mode as the requested global lock mode.
extern const ResourceId resourceIdMultiDocumentTransactionsBarrier;

// Hardcoded resource id for the ReplicationStateTransitionLock (RSTL). This lock is acquired in
// mode X for any replication state transition and is acquired by all other reads and writes in mode
// IX. This lock is acquired after the MultiDocumentTransactionsBarrier lock but before the
// resourceIdGlobal.
extern const ResourceId resourceIdReplicationStateTransitionLock;

/**
 * Interface on which granted lock requests will be notified. See the contract for the notify
 * method for more information and also the LockManager::lock call.
 *
 * The default implementation of this method would simply block on an event until notify has
 * been invoked (see CondVarLockGrantNotification).
 *
 * Test implementations could just count the number of notifications and their outcome so that
 * they can validate locks are granted as desired and drive the test execution.
 */
class LockGrantNotification {
public:
    virtual ~LockGrantNotification() {}

    /**
     * This method is invoked at most once for each lock request and indicates the outcome of
     * the lock acquisition for the specified resource id.
     *
     * Cases where it won't be called are if a pending (not-granted) lock acquisition is cancelled
     * through a call to unlock.
     *
     * IMPORTANT: This callback runs under a spinlock for the lock manager, so the work done
     *            inside must be kept to a minimum and no locks or operations which may block
     *            should be run. Also, no methods which call back into the lock manager should
     *            be invoked from within this methods (LockManager is not reentrant).
     *
     * @resId ResourceId for which a lock operation was previously called.
     * @result Outcome of the lock operation.
     */
    virtual void notify(ResourceId resId, LockResult result) = 0;
};

/**
 * There is one of those entries per each request for a lock. They hang on a linked list off
 * the LockHead or off a PartitionedLockHead and also are in a map for each Locker. This
 * structure is not thread-safe.
 *
 * LockRequest are owned by the Locker class and it controls their lifetime. They should not
 * be deleted while on the LockManager though (see the contract for the lock/unlock methods).
 */
struct LockRequest {
    enum Status : uint8_t {
        STATUS_NEW,
        STATUS_GRANTED,
        STATUS_WAITING,

        // Counts the rest. Always insert new status types above this entry.
        StatusCount
    };

    /**
     * Used for initialization of a LockRequest, which might have been retrieved from cache.
     */
    void initNew(Locker* locker, LockGrantNotification* notify);

    // This is the Locker, which created this LockRequest. Pointer is not owned, just referenced.
    // Must outlive the LockRequest.
    //
    // Written at construction time by Locker
    // Read by LockManager on any thread
    // No synchronization
    Locker* locker;

    // Notification to be invoked when the lock is granted. Pointer is not owned, just referenced.
    // If a request is in the WAITING state, it must live at least until LockManager::unlock is
    // called or the notification has been invoked.
    //
    // Written at construction time by Locker
    // Read by LockManager
    // No synchronization
    LockGrantNotification* notify;

    // If the request cannot be granted right away, whether to put it at the front or at the end of
    // the queue. By default, requests are put at the back. If a request is requested to be put at
    // the front, this effectively bypasses fairness. Default is FALSE.
    //
    // Written at construction time by Locker
    // Read by LockManager on any thread
    // No synchronization
    bool enqueueAtFront : 1;

    // When this request is granted and as long as it is on the granted queue, the particular
    // resource's policy will be changed to "compatibleFirst". This means that even if there are
    // pending requests on the conflict queue, if a compatible request comes in it will be granted
    // immediately. This effectively turns off fairness.
    //
    // Written at construction time by Locker
    // Read by LockManager on any thread
    // No synchronization
    bool compatibleFirst : 1;

    // When set, an attempt is made to execute this request using partitioned lockheads. This speeds
    // up the common case where all requested locking modes are compatible with each other, at the
    // cost of extra overhead for conflicting modes.
    //
    // Written at construction time by LockManager
    // Read by LockManager on any thread
    // No synchronization
    bool partitioned : 1;

    // The current status of this request. Always starts at STATUS_NEW.
    //
    // Written by LockManager on any thread
    // Read by LockManager on any thread
    // Protected by LockHead bucket's mutex
    Status status;

    // If this request is not granted, the mode which has been requested for this lock. If granted,
    // the mode in which it is currently granted.
    //
    // Written by LockManager on any thread
    // Read by LockManager on any thread
    // Protected by LockHead bucket's mutex
    // Read by Locker on Locker thread
    // It is safe for the Locker to read this without taking the bucket mutex provided that the
    // LockRequest status is not WAITING.
    LockMode mode;

    // This unsigned represents the number of pending unlocks for this LockRequest. It is greater
    // than 0 when the LockRequest is participating in two-phase lock and unlock() is called on it.
    // It can be greater than 1 if this lock is participating in two-phase-lock and unlock() has
    // been called multiple times on the same resourceId within the same WriteUnitOfWork. So this
    // value tracks the number of unlocks() to execute at the end of the WUOW.
    //
    // Written by Locker on Locker thread
    // Read by Locker on Locker thread
    // No synchronization
    uint16_t unlockPending;

    // How many times has LockManager::lock been called for this request. Locks are released when
    // their recursive count drops to zero.
    //
    // Written by LockManager on Locker thread
    // Read by LockManager on Locker thread
    // Read by Locker on Locker thread
    // No synchronization
    uint16_t recursiveCount;

    // Pointer to the lock to which this request belongs, or null if this request has not yet been
    // assigned to a lock or if it belongs to the PartitionedLockHead for locker (in which case
    // partitionedLock must be set). The LockHead should be alive as long as there are LockRequests
    // on it, so it is safe to have this pointer hanging around.
    //
    // Written by LockManager on any thread
    // Read by LockManager on any thread
    // Protected by LockHead bucket's mutex
    LockHead* lock;

    // Pointer to the partitioned lock to which this request belongs, or null if it is not
    // partitioned. Only one of 'lock' and 'partitionedLock' is non-NULL, and a request can only
    // transition from 'partitionedLock' to 'lock', never the other way around.
    //
    // Written by LockManager on any thread
    // Read by LockManager on any thread
    // Protected by LockHead bucket's mutex
    PartitionedLockHead* partitionedLock;

    // The linked list chain on which this request hangs off the owning lock head. The reason
    // intrusive linked list is used instead of the std::list class is to allow for entries to be
    // removed from the middle of the list in O(1) time, if they are known instead of having to
    // search for them and we cannot persist iterators, because the list can be modified while an
    // iterator is held.
    //
    // Written by LockManager on any thread
    // Read by LockManager on any thread
    // Protected by LockHead bucket's mutex
    LockRequest* prev;
    LockRequest* next;
};

/**
 * Type used to fetch lock info from the LockManager for debugging purposes.
 * Note that using a struct to fetch internal LockManager information is preferable than a BSONObj
 * to minimize the time the LockManager mutexes are hold.
 */
struct LogDebugInfo {
    LogDebugInfo(LockMode mode, const std::string& debugInfo) : mode(mode), debugInfo(debugInfo) {}

    LockMode mode;
    std::string debugInfo;
};

/**
 * Returns a human readable status name for the specified LockRequest status.
 */
const char* lockRequestStatusName(LockRequest::Status status);

}  // namespace mongo
