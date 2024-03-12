/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <thread>

#include "mongo/db/admission/throughput_probing.h"
#include "mongo/tools/workload_simulation/simulation.h"
#include "mongo/tools/workload_simulation/throughput_probing/ticketed_workload_driver.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/mock_periodic_runner.h"

namespace mongo::workload_simulation {

/**
 * This class sets up the ticket holders, 'ThroughputProbing' monitor, then runs a simulated
 * workload driver. Users specify their own 'TicketedWorkloadDriver' instance to the 'run' method,
 * and run the workload for a desired duration.
 */
class ThroughputProbing : public Simulation {
public:
    ThroughputProbing(StringData workloadName);

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
    std::unique_ptr<admission::ThroughputProbing> _throughputProbing;
    stdx::thread _probingThread;
    AtomicWord<bool> _probing;

    std::unique_ptr<TicketedWorkloadDriver> _driver;
    AtomicWord<bool> _running;
};

}  // namespace mongo::workload_simulation
