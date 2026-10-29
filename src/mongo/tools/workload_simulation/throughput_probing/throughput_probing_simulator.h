// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/execution_control/throughput_probing.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/tools/workload_simulation/simulation.h"
#include "mongo/tools/workload_simulation/throughput_probing/ticketed_workload_driver.h"
#include "mongo/util/mock_periodic_runner.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <thread>

namespace mongo::workload_simulation {

/**
 * This class sets up the ticket holders, 'ThroughputProbing' monitor, then runs a simulated
 * workload driver. Users specify their own 'TicketedWorkloadDriver' instance to the 'run' method,
 * and run the workload for a desired duration.
 */
class ThroughputProbing : public Simulation {
public:
    ThroughputProbing(std::string_view workloadName);

    void setup() override;
    void teardown() override;
    size_t actorCount() const override;
    boost::optional<BSONObj> metrics() const override;

    /**
     * Begins running the specified 'driver' as configured.
     *
     * @param driver        The workload driver to be run.
     * @param numReaders    Number of read actor threads for the workload.
     * @param numWriters    Number of write actor threads for the workload.
     */
    void start(std::unique_ptr<TicketedWorkloadDriver>&& driver,
               int32_t numReaders,
               int32_t numWriters);

    /**
     * Changes the number of threads for the workload.
     *
     * @param numReaders    Number of read actor threads for the workload.
     * @param numWriters    Number of write actor threads for the workload.
     */
    void resize(int32_t numReaders, int32_t numWriters);

    /**
     * Runs for the specified'runTime'.
     *
     * @param runTime       The duration to run the workload for.
     */
    void run(Seconds runTime);

private:
    std::unique_ptr<TicketHolder> _readTicketHolder;
    std::unique_ptr<TicketHolder> _writeTicketHolder;

    MockPeriodicRunner* _runner;
    std::unique_ptr<admission::execution_control::ThroughputProbing> _throughputProbing;
    stdx::thread _probingThread;
    Atomic<bool> _probing;

    std::unique_ptr<TicketedWorkloadDriver> _driver;
    Atomic<bool> _running;
};

}  // namespace mongo::workload_simulation
