// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

/**
 * Allocation time memory charging.
 *
 * Dependency free on purpose, so that low level libraries (document_value, bson, util) can include
 * it without depending on db/memory_tracking.
 *
 * An allocation site charges before it allocates and releases after it frees, passing the same
 * byte count both times. The tracker keeps a running total, not a record of individual
 * allocations, so matching each release to its charge is the allocation site's responsibility.
 *
 * How the tracker is found: from ambient state, i.e. a thread local set with an RAII guard by
 * whoever owns a tracker. A null ambient means untracked, so an allocation made outside any guarded
 * scope behaves exactly as it does without tracking.
 */
namespace [[MONGO_MOD_PUBLIC]] mongo {

class [[MONGO_MOD_OPEN]] MemoryCharging {
public:
    // Virtual, not protected, because the trackers that implement this interface are themselves
    // deleted polymorphically, e.g. OperationMemoryUsageTracker held in a unique_ptr.
    virtual ~MemoryCharging() = default;

    /**
     * It charges 'bytes' to the running total of the inUse memory bytes tracked by the tracker. It
     * should be called before the allocation of 'bytes' of memory.
     */
    virtual void chargeBytes(int64_t bytes) = 0;

    /**
     * It releases 'bytes' from the running total of the inUse memory bytes tracked by the tracker.
     * It should be called after the free of 'bytes' of memory, with the same size that was charged
     * for that memory.
     */
    virtual void releaseBytes(int64_t bytes) = 0;
};

/**
 * The ambient ("current") charging target: what 'chargeAmbient()' and 'releaseAmbient()' use. Null
 * means untracked. Only the ambient charger charges; the ones installed before it keep what they
 * have been charged and do not see new charges until they are ambient again.
 */
[[MONGO_MOD_FILE_PRIVATE]] inline thread_local MemoryCharging* gAmbientMemoryCharging = nullptr;

inline MemoryCharging* currentMemoryCharging() {
    return gAmbientMemoryCharging;
}

/**
 * Installs 'charging' as the ambient charger for a scope. The charger that was ambient is saved in
 * the guard and becomes ambient again when the guard is destroyed, so scopes nest to any depth and
 * the same charger may be installed again while it is buried. Guards must be destroyed in the
 * reverse order of their construction, which scoped use guarantees.
 *
 * Installing nullptr suspends charging for the scope, for crossing a boundary past which
 * allocations must not be charged. Memory allocated on one side of the boundary and freed on the
 * other is charged and released against different chargers, so the boundary should only contain
 * memory that lives and dies inside it.
 */
class AmbientMemoryChargingGuard {
public:
    explicit AmbientMemoryChargingGuard(MemoryCharging* charging)
        : _previous(gAmbientMemoryCharging) {
        gAmbientMemoryCharging = charging;
    }

    ~AmbientMemoryChargingGuard() {
        gAmbientMemoryCharging = _previous;
    }

    AmbientMemoryChargingGuard(const AmbientMemoryChargingGuard&) = delete;
    AmbientMemoryChargingGuard& operator=(const AmbientMemoryChargingGuard&) = delete;

private:
    MemoryCharging* _previous;
};

/**
 * It charges 'bytes' to the running total of the inUse memory bytes tracked by the ambient charger,
 * and does nothing if there is none. It should be called before the allocation of 'bytes' of
 * memory.
 */
inline void chargeAmbient(int64_t bytes) {
    if (auto* charging = gAmbientMemoryCharging) {
        charging->chargeBytes(bytes);
    }
}

/**
 * It releases 'bytes' from the running total of the inUse memory bytes tracked by the ambient
 * charger, and does nothing if there is none. It should be called after the free of 'bytes' of
 * memory, with the same size that was charged for that memory. It should run with the same ambient
 * charger as the corresponding chargeAmbient(), otherwise the release does not reach the charger
 * that was charged.
 */
inline void releaseAmbient(int64_t bytes) {
    if (auto* charging = gAmbientMemoryCharging) {
        charging->releaseBytes(bytes);
    }
}

}  // namespace mongo
