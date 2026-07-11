// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
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
class [[MONGO_MOD_PUBLIC]] FlowControl {
public:
    class [[MONGO_MOD_OPEN]] TimestampProvider {
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

        /**
         * Returns the number of ops the sustainer applied in the last period, bypassing
         * _approximateOpsBetween(). Returns -1 by default to indicate that the caller should
         * fall back to _approximateOpsBetween() for the count. Overridden by providers whose
         * sustainer timestamps live in a different domain than the oplog samples (e.g. phylog
         * LSNs in disaggregated storage).
         */
        virtual std::int64_t getSustainerAppliedCount() const {
            return -1;
        }
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
     * Returns an estimate of the number of oplog operations between two timestamps by querying
     * the internal samples table.
     */
    std::int64_t approximateOpsBetween(Timestamp prevTs, Timestamp currTs);

    /**
     * Underscore methods are public for testing.
     */
    [[MONGO_MOD_PRIVATE]] std::int64_t _getLocksUsedLastPeriod();
    [[MONGO_MOD_PRIVATE]] double _getLocksPerOp();

    [[MONGO_MOD_PRIVATE]] std::int64_t _approximateOpsBetween(Timestamp prevTs, Timestamp currTs);

    [[MONGO_MOD_PRIVATE]] int _calculateNewTicketsForLag(const Timestamp& prevSustainerTimestamp,
                                                         const Timestamp& currSustainerTimestamp,
                                                         std::int64_t locksUsedLastPeriod,
                                                         double locksPerOp,
                                                         std::uint64_t lagMillis,
                                                         std::uint64_t thresholdLagMillis);

    [[MONGO_MOD_PRIVATE]] void _trimSamples(Timestamp trimSamplesTo);

    // Sample of (timestamp, ops, lock acquisitions) where ops and lock acquisitions are
    // observations of the corresponding counter at (roughly) <timestamp>.
    typedef std::tuple<std::uint64_t, std::uint64_t, std::int64_t> Sample;
    [[MONGO_MOD_PRIVATE]] const std::deque<Sample>& _getSampledOpsApplied_forTest() {
        return _sampledOpsApplied;
    }

private:
    std::unique_ptr<TimestampProvider> _timestampProvider;

    // These values are updated with each flow control computation and are also surfaced in server
    // status.
    Atomic<int> _lastTargetTicketsPermitted{kMaxTickets};
    Atomic<double> _lastLocksPerOp{0.0};
    Atomic<int> _lastSustainerAppliedCount{0};
    Atomic<bool> _isLagged{false};
    Atomic<int> _isLaggedCount{0};
    // Use an int64_t as this is serialized to bson which does not support unsigned 64-bit numbers.
    Atomic<std::int64_t> _isLaggedTimeMicros{0};
    Atomic<Date_t> _disableUntil;

    mutable std::mutex _sampledOpsMutex;
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
class [[MONGO_MOD_PUBLIC]] ReplicationTimestampProvider final
    : public FlowControl::TimestampProvider {
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
