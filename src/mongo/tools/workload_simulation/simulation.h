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

#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/tools/workload_simulation/event_queue.h"

#include <memory>
#include <set>
#include <string>

#define CLASS_NAME(SUITE, WORKLOAD) SUITE##_##WORKLOAD

/**
 * Used similarly to TEST_F to define a simulation workload. The 'SUITE' should be a derivative of
 * 'Simulation', and will be the base class for the simulation. Any additional parameters after the
 * 'WORKLOAD' name will be passed as arguments to the 'SUITE' constructor after, after the
 * stringized 'WORKLOAD'.
 */
#define SIMULATION(SUITE, WORKLOAD)                                   \
    class CLASS_NAME(SUITE, WORKLOAD) : public SUITE {                \
    public:                                                           \
        CLASS_NAME(SUITE, WORKLOAD)() : SUITE(#WORKLOAD) {}           \
        void _doRun() override;                                       \
                                                                      \
    private:                                                          \
        struct RegistrationAgent {                                    \
            RegistrationAgent() {                                     \
                SimulationRegistry::get().registerSimulation(         \
                    std::make_unique<CLASS_NAME(SUITE, WORKLOAD)>()); \
            }                                                         \
        };                                                            \
        static inline RegistrationAgent _agent;                       \
    };                                                                \
    void CLASS_NAME(SUITE, WORKLOAD)::_doRun()

namespace mongo::workload_simulation {

/**
 * This is the base class for all workload simulations. It establishes and provides access to a
 * 'ServiceContext' (with initialized 'TickSourceMock'), a 'Client', and an 'EventQueue'.
 *
 * When constructed via the 'SIMULATION' macro, the function body provided after the macro will
 * override _doRun().
 */
class Simulation {
public:
    Simulation() = delete;
    Simulation(StringData suiteName, StringData workloadName);
    virtual ~Simulation();

    /**
     * Called once prior to running. Any global state for the simulation should be set up here
     * rather than in the constructor. This base implementation should be called from any
     * override.
     */
    virtual void setup();

    /**
     * Called once after running. Any global state for the simulation should be torn down here
     * rather than in the destructor. This base implementation should be called from any
     * override.
     */
    virtual void teardown();

    /**
     * Override automatically generated for derived classes by 'SIMULATION' macro. The function body
     * provided after the macro invocation will constitute the body of this override function. This
     * function should send events to the queue, being careful to manage the lifetime of any data
     * those events depend on, as they may outlive this method.
     */
    virtual void _doRun();

    /**
     * This method will be passed to the 'EventQueue' constructor as the 'actorCount' parameter.
     */
    virtual size_t actorCount() const = 0;

    /**
     * Report metrics to be plotted.
     *
     * Each top-level field will generate a new sub-plot, and all second-level fields under a given
     * top-level field will share a plot. For instance, to plot the optimal vs. allocated ticket
     * counts for read and write separately, return a document like:
     * {
     *   read: { optimal: 10, allocated: 5 }
     *   write: { optimal: 10, allocated:  5 }
     * }
     */
    virtual boost::optional<BSONObj> metrics() const = 0;

    /**
     * This is the primary method called when a workload is run. It calls 'setup', which begins
     * processing the queue and begins monitoring, executes the simulated workload, transitions the
     * queue into a draining mode, then runs 'teardown'.
     */
    void run();

    /**
     * Name of the suite the simulated workload belongs to.
     */
    std::string suiteName() const;

    /**
     * Name of the simulated workload.
     */
    std::string workloadName() const;

protected:
    /**
     * Returns the underlying event queue.
     */
    EventQueue& queue();

    /**
     * Returns the service context.
     */
    ServiceContext* svcCtx();

    /**
     * Returns a client for use by the derived class.
     */
    Client* client();

private:
    std::string _suiteName;
    std::string _workloadName;

    ServiceContext::UniqueServiceContext _svcCtx;
    ServiceContext::UniqueClient _client;

    TickSourceMock<Nanoseconds>* _tickSource;
    std::unique_ptr<EventQueue> _eventQueue;
    stdx::thread _monitor;
    AtomicWord<bool> _stopping{false};
};

class SimulationRegistry {
public:
    /**
     * Returns the global singleton instance of 'SimulationRegistry'.
     */
    static SimulationRegistry& get();

    /**
     * Runs selected workloads.
     */
    void runSelected();

    /**
     * Registers a workload.
     */
    void registerSimulation(std::unique_ptr<Simulation>&& simulation);

    /**
     * Prints a list of all suites that have been registered.
     */
    void list() const;

private:
    struct NameLess {
        bool operator()(const std::unique_ptr<Simulation>& lhs,
                        const std::unique_ptr<Simulation>& rhs) const {
            return lhs->suiteName() < rhs->suiteName() ||
                (lhs->suiteName() == rhs->suiteName() && lhs->workloadName() < rhs->workloadName());
        }
    };
    std::set<std::unique_ptr<Simulation>, NameLess> _workloads;
};

}  // namespace mongo::workload_simulation
