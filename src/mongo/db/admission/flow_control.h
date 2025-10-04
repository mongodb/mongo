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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_fwd.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <tuple>
#include <vector>

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
    class TimestampProvider {
    public:
        virtual ~TimestampProvider() = default;
        /**
         * The sustainer timestamp is the timestamp which, if moved forward, will cause an
         * advance in the target timestamp.  For replication, it is the median applied timestamp
         * on all the relevant nodes.  We need to know this timestamp both for the current iteration
         * and the previous iteration.
         */
        virtual Timestamp getCurrSustainerTimestamp() const = 0;
        virtual Timestamp getPrevSustainerTimestamp() const = 0;

        /**
         * The target time is the time we are trying to throttle to.  For replication, it is the
         * last committed time (majority snapshot time).
         */
        virtual repl::TimestampAndWallTime getTargetTimestampAndWallTime() const = 0;

        /**
         * The last write time is what we are trying to control.  For replication, it is
         * the last applied time.
         */
        virtual repl::TimestampAndWallTime getLastWriteTimestampAndWallTime() const = 0;

        /**
         * Is flow control possible with this timestamp provider?  For replication,
         * true if this is a primary and majority read concern is enabled.
         */
        virtual bool flowControlUsable() const = 0;

        /**
         * Are the previous and current updates compatible?  For replication,
         * makes sure number of nodes is the same and the median node timestamp (the sustainer)
         * has not gone backwards.
         */
        virtual bool sustainerAdvanced() const = 0;

        /**
         * Advance the `_*MemberData` fields and sort the new data by the element's last applied
         * optime.
         */
        virtual void update() = 0;
    };

    static constexpr int kMaxTickets = 1000 * 1000 * 1000;

    /**
     * Construct a flow control object based on a custom timestamp provider.
     * Takes ownership of the timestamp provider.
     */
    FlowControl(ServiceContext* service, std::unique_ptr<TimestampProvider> timestampProvider);

    /**
     * Construct a replication-based flow control object.
     */
    FlowControl(ServiceContext* service, repl::ReplicationCoordinator* replCoord);

    /**
     * Construct a replication-based flow control object without adding a periodic job runner for
     * testing.
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

    int _calculateNewTicketsForLag(const Timestamp& prevSustainerTimestamp,
                                   const Timestamp& currSustainerTimestamp,
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
    std::unique_ptr<TimestampProvider> _timestampProvider;

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

    mutable stdx::mutex _sampledOpsMutex;
    std::deque<Sample> _sampledOpsApplied;

    // These values are used in the sampling process.
    std::uint64_t _numOpsSinceStartup = 0;
    std::uint64_t _lastSample = 0;

    std::int64_t _lastPollLockAcquisitions = 0;

    Date_t _lastTimeSustainerAdvanced;

    // This value is used for calculating server status metrics.
    std::uint64_t _startWaitTime = 0;

    PeriodicJobAnchor _jobAnchor;
};

namespace flow_control_details {
class ReplicationTimestampProvider final : public FlowControl::TimestampProvider {
public:
    explicit ReplicationTimestampProvider(repl::ReplicationCoordinator* replCoord);
    Timestamp getCurrSustainerTimestamp() const final;
    Timestamp getPrevSustainerTimestamp() const final;
    repl::TimestampAndWallTime getTargetTimestampAndWallTime() const final;
    repl::TimestampAndWallTime getLastWriteTimestampAndWallTime() const final;
    bool flowControlUsable() const final;
    bool sustainerAdvanced() const final;
    void update() final;

    void setCurrMemberData_forTest(const std::vector<repl::MemberData>& memberData);
    void setPrevMemberData_forTest(const std::vector<repl::MemberData>& memberData);

private:
    repl::ReplicationCoordinator* _replCoord;
    std::vector<repl::MemberData> _currMemberData;
    std::vector<repl::MemberData> _prevMemberData;
};

}  // namespace flow_control_details

}  // namespace mongo
