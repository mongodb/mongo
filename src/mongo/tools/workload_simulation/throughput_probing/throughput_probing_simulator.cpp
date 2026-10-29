// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/throughput_probing/throughput_probing_simulator.h"

#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/throughput_probing.h"
#include "mongo/db/admission/execution_control/throughput_probing_gen.h"
#include "mongo/db/admission/ticketing/ticketholder.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::workload_simulation {

ThroughputProbing::ThroughputProbing(std::string_view workloadName)
    : Simulation("ThroughputProbing", workloadName) {}

void ThroughputProbing::setup() {
    Simulation::setup();

    const auto initialTickets =
        admission::execution_control::throughput_probing::gInitialConcurrency;
    constexpr auto initialMaxQueueDepth = TicketHolder::kDefaultMaxQueueDepth;

    constexpr bool trackPeakUsed = true;
    _readTicketHolder = std::make_unique<TicketHolder>(
        svcCtx(), initialTickets, trackPeakUsed, initialMaxQueueDepth);
    _writeTicketHolder = std::make_unique<TicketHolder>(
        svcCtx(), initialTickets, trackPeakUsed, initialMaxQueueDepth);

    _runner = [svcCtx = svcCtx()] {
        auto runner = std::make_unique<MockPeriodicRunner>();
        auto runnerPtr = runner.get();
        svcCtx->setPeriodicRunner(std::move(runner));
        return runnerPtr;
    }();

    _throughputProbing = std::make_unique<admission::execution_control::ThroughputProbing>(
        svcCtx(), _readTicketHolder.get(), _writeTicketHolder.get());

    _probingThread = stdx::thread([this]() {
        _probing.store(true);
        while (_probing.load()) {
            if (queue().wait_for(Milliseconds{admission::execution_control::throughput_probing::
                                                  gConcurrencyAdjustmentIntervalMillis.load()},
                                 EventQueue::WaitType::Observer)) {
                _runner->run(client());
            }
        }
    });
}

void ThroughputProbing::teardown() {
    _running.store(false);
    _driver->stop();

    Simulation::teardown();

    _driver.reset();

    _probing.store(false);
    _probingThread.join();

    _throughputProbing.reset();
    _writeTicketHolder.reset();
    _readTicketHolder.reset();
    _runner = nullptr;
}

size_t ThroughputProbing::actorCount() const {
    size_t count = 0;
    if (_readTicketHolder->queued() > 0) {
        count += _readTicketHolder->outof();
    } else {
        count += _readTicketHolder->used();
    }
    if (_writeTicketHolder->queued() > 0) {
        count += _writeTicketHolder->outof();
    } else {
        count += _writeTicketHolder->used();
    }
    // Due to the way the ticket holder implementation resizes down, we have to
    // add a fudge factor here. Otherwise, we could encounter a deadlock, as the
    // ticket holder may be waiting to acquire tickets to burn, while they are
    // held by threads waiting in our queue. The holder implementation only
    // updates the outof() value once it has finished burning all the
    // disappearing tickets.
    return count *
        (1 - admission::execution_control::throughput_probing::gStepMultiple.loadRelaxed());
}

boost::optional<BSONObj> ThroughputProbing::metrics() const {
    if (!_running.load()) {
        return boost::none;
    }
    return _driver->metrics();
}

void ThroughputProbing::start(std::unique_ptr<TicketedWorkloadDriver>&& driver,
                              int32_t numReaders,
                              int32_t numWriters) {
    // Start up.
    _driver = std::move(driver);
    _driver->start(
        svcCtx(), _readTicketHolder.get(), _writeTicketHolder.get(), numReaders, numWriters);
    _running.store(true);
}

void ThroughputProbing::resize(int32_t numReaders, int32_t numWriters) {
    invariant(_running.load());
    _driver->resize(numReaders, numWriters);
}

void ThroughputProbing::run(Seconds runTime) {
    invariant(_running.load());
    queue().wait_for(runTime, EventQueue::WaitType::Observer);
}


}  // namespace mongo::workload_simulation
