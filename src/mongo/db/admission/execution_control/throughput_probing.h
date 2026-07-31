// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace admission {
namespace execution_control {
namespace [[MONGO_MOD_PUBLIC]] throughput_probing {

class [[MONGO_MOD_PRIVATE]] ThroughputProbingTest;
class [[MONGO_MOD_PRIVATE]] InitStateWarningTest;

[[MONGO_MOD_PRIVATE]] Status validateInitialConcurrency(int32_t concurrency,
                                                        const boost::optional<TenantId>&);
[[MONGO_MOD_PRIVATE]] Status validateMinConcurrency(int32_t concurrency,
                                                    const boost::optional<TenantId>&);
[[MONGO_MOD_PRIVATE]] Status validateMaxConcurrency(int32_t concurrency,
                                                    const boost::optional<TenantId>&);

/**
 * on_update callback for the throughputProbingConcurrencyAdjustmentIntervalMillis parameter.
 * Updates the throughput probing periodic job's interval.
 */
Status onUpdateConcurrencyAdjustmentIntervalMillis(const int32_t& newValue);

}  // namespace throughput_probing

/**
 * Adjusts the level of concurrency on the read and write ticket holders by probing up/down and
 * attempting to maximize throughput. Assumes both ticket holders have the same starting
 * concurrency level and always keeps the same concurrency level for both.
 */
class [[MONGO_MOD_PUBLIC]] ThroughputProbing {
public:
    ThroughputProbing(ServiceContext* svcCtx,
                      TicketHolder* readTicketHolder,
                      TicketHolder* writeTicketHolder);

    void appendStats(BSONObjBuilder& builder) const;

    void start();

    void stop();

    /**
     * Sets the period for the throughput probing periodic job.
     */
    void setPeriod(Milliseconds period);

    /**
     * Gets the period for the throughput probing periodic job.
     */
    Milliseconds getPeriod() const;

private:
    friend class throughput_probing::ThroughputProbingTest;
    friend class throughput_probing::InitStateWarningTest;

    enum class ProbingState {
        kStable,
        kUp,
        kDown,
    };

    void _run(Client*);

    void _probeStable(OperationContext* opCtx, double throughput);
    void _probeUp(OperationContext* opCtx, double throughput);
    void _probeDown(OperationContext* opCtx, double throughput);

    void _resetConcurrency(OperationContext* opCtx);
    void _increaseConcurrency(OperationContext* opCtx);
    void _decreaseConcurrency(OperationContext* opCtx);

    void _resize(OperationContext* opCtx, TicketHolder* ticketholder, int newTickets);

    void _initState();

    ServiceContext* _svcCtx;

    TicketHolder* _readTicketHolder;
    TicketHolder* _writeTicketHolder;

    // This value is split between reads and writes based on the read/write ratio.
    double _stableConcurrency = 0;
    double _stableThroughput = 0;
    ProbingState _state = ProbingState::kStable;
    int64_t _prevNumFinishedProcessing = -1;

    Timer _timer;

    struct Stats {
        void serialize(BSONObjBuilder& builder) const;

        Atomic<int64_t> timesDecreased;
        Atomic<int64_t> timesIncreased;
        Atomic<int64_t> totalAmountDecreased;
        Atomic<int64_t> totalAmountIncreased;
        Atomic<int64_t> resizeDurationMicros;
        Atomic<int64_t> timesProbedStable;
        Atomic<int64_t> timesProbedUp;
        Atomic<int64_t> timesProbedDown;
    } _stats;

    mutable std::mutex _mutex;

    PeriodicJobAnchor _job;
};

}  // namespace execution_control
}  // namespace admission
}  // namespace mongo
