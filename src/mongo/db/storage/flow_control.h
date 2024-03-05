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

#include <cstdint>
#include <deque>
#include <memory>
#include <tuple>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_fwd.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * This class encapsulates (most) logic relating to throttling incoming writes when a primary
 * discovers the commit point is lagging behind. The only method exposed to the system for
 * functionality is the `sample` method. On a primary, replication will call that method every time
 * new optimes are generated. FlowControl uses that to keep a data structure that can approximately
 * answer the question: "How many operations are between two timestamps?"
 *
 * Otherwise this class' only output is to refresh the tickets available in the
 * `FlowControlTicketholder`.
 */
class FlowControl {
public:
    static constexpr int kMaxTickets = 1000 * 1000 * 1000;

    FlowControl(ServiceContext* service, repl::ReplicationCoordinator* replCoord);

    /**
     * Construct a flow control object without adding a periodic job runner for testing.
     */
    FlowControl(repl::ReplicationCoordinator* replCoord);

    static FlowControl* get(ServiceContext* service);
    static FlowControl* get(ServiceContext& service);
    static FlowControl* get(OperationContext* ctx);

    static void set(ServiceContext* service, std::unique_ptr<FlowControl> flowControl);

    /**
     * Shuts down the flow control job and removes it from the ServiceContext.
     */
    static void shutdown(ServiceContext* service);

    /*
     * Typical API call.
     *
     * Calculates how many tickets should be handed out in the next interval. If there's no majority
     * point lag, the number of tickets should increase. If there is majority point lag beyond a
     * threshold, the number of granted tickets is derived from how much progress secondaries are
     * making.
     *
     * If Flow Control is disabled via `disabledUntil`, return the maximum number of tickets.
     */
    int getNumTickets() {
        return getNumTickets(Date_t::now());
    }

    /**
     * Exposed for testing.
     */
    int getNumTickets(Date_t now);

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const;

    /**
     * This method is called when replication is reserving `opsApplied` timestamps. `timestamp` is
     * the timestamp in the oplog associated with the first oplog time being reserved.
     */
    void sample(Timestamp timestamp, std::uint64_t opsApplied);

    /**
     * Disables flow control until `deadline` is reached.
     */
    void disableUntil(Date_t deadline);

    /**
     * Underscore methods are public for testing.
     */
    std::int64_t _getLocksUsedLastPeriod();
    double _getLocksPerOp();

    std::int64_t _approximateOpsBetween(Timestamp prevTs, Timestamp currTs);

    void _updateTopologyData();
    int _calculateNewTicketsForLag(const std::vector<repl::MemberData>& prevMemberData,
                                   const std::vector<repl::MemberData>& currMemberData,
                                   std::int64_t locksUsedLastPeriod,
                                   double locksPerOp,
                                   std::uint64_t lagMillis,
                                   std::uint64_t thresholdLagMillis);
    void _trimSamples(Timestamp trimSamplesTo);

    // Sample of (timestamp, ops, lock acquisitions) where ops and lock acquisitions are
    // observations of the corresponding counter at (roughly) <timestamp>.
    typedef std::tuple<std::uint64_t, std::uint64_t, std::int64_t> Sample;
    const std::deque<Sample>& _getSampledOpsApplied_forTest() {
        return _sampledOpsApplied;
    }

private:
    repl::ReplicationCoordinator* _replCoord;

    // These values are updated with each flow control computation and are also surfaced in server
    // status.
    AtomicWord<int> _lastTargetTicketsPermitted{kMaxTickets};
    AtomicWord<double> _lastLocksPerOp{0.0};
    AtomicWord<int> _lastSustainerAppliedCount{0};
    AtomicWord<bool> _isLagged{false};
    AtomicWord<int> _isLaggedCount{0};
    // Use an int64_t as this is serialized to bson which does not support unsigned 64-bit numbers.
    AtomicWord<std::int64_t> _isLaggedTimeMicros{0};
    AtomicWord<Date_t> _disableUntil;

    mutable Mutex _sampledOpsMutex = MONGO_MAKE_LATCH("FlowControl::_sampledOpsMutex");
    std::deque<Sample> _sampledOpsApplied;

    // These values are used in the sampling process.
    std::uint64_t _numOpsSinceStartup = 0;
    std::uint64_t _lastSample = 0;

    std::int64_t _lastPollLockAcquisitions = 0;

    std::vector<repl::MemberData> _currMemberData;
    std::vector<repl::MemberData> _prevMemberData;

    Date_t _lastTimeSustainerAdvanced;

    // This value is used for calculating server status metrics.
    std::uint64_t _startWaitTime = 0;

    PeriodicJobAnchor _jobAnchor;
};

}  // namespace mongo
