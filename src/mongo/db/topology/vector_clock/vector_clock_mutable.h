// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

/**
 * A vector clock service that additionally permits being advanced authoritatively ("ticking").
 *
 * Only linked in contexts where ticking is allowed, ie. mongod, embedded, mongod-based unittests.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] VectorClockMutable : public VectorClock {
public:
    // Decorate ServiceContext with VectorClockMutable*, that will resolve to the mutable vector
    // clock implementation.
    static VectorClockMutable* get(ServiceContext* service);
    static VectorClockMutable* get(OperationContext* ctx);

    static void registerVectorClockOnServiceContext(ServiceContext* service,
                                                    VectorClockMutable* vectorClockMutable);

    /**
     * Returns the next time value for the component, and provides a guarantee that any future call
     * to tick() will return a value at least 'nTicks' ticks in the future from the current time.
     */
    LogicalTime tickClusterTime(uint64_t nTicks) {
        return _tick(Component::ClusterTime, nTicks);
    }
    LogicalTime tickConfigTime(uint64_t nTicks) {
        return _tick(Component::ConfigTime, nTicks);
    }
    LogicalTime tickTopologyTime(uint64_t nTicks) {
        return _tick(Component::TopologyTime, nTicks);
    }

    /**
     * Authoritatively ticks the current time of the specified component to newTime.
     *
     * For ClusterTime, this should only be used for initializing from a trusted source, eg. from an
     * oplog timestamp.
     */
    void tickClusterTimeTo(LogicalTime newTime) {
        _tickTo(Component::ClusterTime, newTime);
    }
    void tickConfigTimeTo(LogicalTime newTime) {
        _tickTo(Component::ConfigTime, newTime);
    }
    void tickTopologyTimeTo(LogicalTime newTime) {
        _tickTo(Component::TopologyTime, newTime);
    }

    /**
     * These methods ensure that the values of the specified vector clock components as of the time
     * of the call have been durably persisted to disk, before setting the returned future.
     * Persisting the vector clock ensures that subsequent calls to `recover()` below will bring the
     * components to at least the persisted time.
     */
    virtual SharedSemiFuture<void> waitForDurableConfigTime() = 0;
    virtual SharedSemiFuture<void> waitForDurableTopologyTime() = 0;
    virtual SharedSemiFuture<void> waitForDurable() = 0;

    /**
     * Ensures that the values of the vector clock are at least equal to those from the last
     * successfully persisted ones.
     */
    virtual VectorClock::VectorTime recoverDirect(OperationContext* opCtx) = 0;

protected:
    VectorClockMutable();
    ~VectorClockMutable() override;

    /**
     * Called by sub-classes in order to actually tick a Component time, once they have determined
     * that doing so is permissible.
     *
     * Returns as per tick(), ie. returns the next time value, and guarantees that future calls will
     * return at least nTicks later.
     */
    LogicalTime _advanceComponentTimeByTicks(Component component, uint64_t nTicks);

    /**
     * Called by subclasses in order to actually tickTo a Component time, once they have determined
     * that doing so is permissible.
     */
    void _advanceComponentTimeTo(Component component, LogicalTime&& newTime);

    /**
     * Returns the next time value for the component, and provides a guarantee that any future call
     * to tick() will return a value at least 'nTicks' ticks in the future from the current time.
     */
    virtual LogicalTime _tick(Component component, uint64_t nTicks) = 0;

    /**
     * Authoritatively ticks the current time of the Component to newTime.
     *
     * For ClusterTime, this should only be used for initializing from a trusted source, eg. from an
     * oplog timestamp.
     */
    virtual void _tickTo(Component component, LogicalTime newTime) = 0;
};

}  // namespace mongo
