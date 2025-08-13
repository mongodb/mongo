/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/util/future.h"

#include <cstdint>

namespace mongo {

/**
 * A vector clock service that additionally permits being advanced authoritatively ("ticking").
 *
 * Only linked in contexts where ticking is allowed, ie. mongod, embedded, mongod-based unittests.
 */
class VectorClockMutable : public VectorClock {
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
     * Called by sublclasses in order to actually tickTo a Component time, once they have determined
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
