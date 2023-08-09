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

#include "mongo/tools/workload_simulation/simulation.h"

#include "mongo/db/concurrency/locker_impl.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/tools/workload_simulation/simulator_options.h"
#include "mongo/util/pcre.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::workload_simulation {
namespace {
bool shouldRun(const Simulation& simulation) {
    auto& suiteAllowList = simulatorGlobalParams.suites;
    if (!suiteAllowList.empty() &&
        std::find(suiteAllowList.begin(), suiteAllowList.end(), simulation.suiteName()) ==
            suiteAllowList.end()) {
        LOGV2_DEBUG(7782102,
                    3,
                    "Skipped workload due to suite exclusion",
                    "suite"_attr = simulation.suiteName(),
                    "workload"_attr = simulation.workloadName());
        return false;
    }

    auto& workloadFilter = simulatorGlobalParams.filter;
    if (!workloadFilter.empty()) {
        pcre::Regex workloadFilterRe{workloadFilter};
        if (!workloadFilterRe.matchView(simulation.workloadName())) {
            LOGV2_DEBUG(7782103,
                        3,
                        "Skipped workload due to filter",
                        "suite"_attr = simulation.suiteName(),
                        "workload"_attr = simulation.workloadName());
            return false;
        }
    }

    return true;
}

class LockerClientObserver : public ServiceContext::ClientObserver {
public:
    LockerClientObserver() = default;
    ~LockerClientObserver() = default;

    void onCreateClient(Client* client) final {}
    void onDestroyClient(Client* client) final {}
    void onCreateOperationContext(OperationContext* opCtx) final {
        auto service = opCtx->getServiceContext();

        opCtx->setLockState(std::make_unique<LockerImpl>(service));
    }
    void onDestroyOperationContext(OperationContext* opCtx) final {}
};
}  // namespace

Simulation::Simulation(StringData suiteName, StringData workloadName)
    : _suiteName(suiteName.toString()), _workloadName(workloadName.toString()) {}

Simulation::~Simulation() {}

void Simulation::setup() {
    _svcCtx = ServiceContext::make();
    _client = _svcCtx->makeClient(suiteName());

    _tickSource = initTickSourceMock<ServiceContext, Nanoseconds>(_svcCtx.get());
    _eventQueue = std::make_unique<EventQueue>(*_tickSource, [this]() {
        // Make the queue wait to process events until it has the same number of events queued as we
        // have busy actors. Otherwise it may advance the clock before we have a chance to queue our
        // events.
        return actorCount();
    });

    _svcCtx->registerClientObserver(std::make_unique<LockerClientObserver>());

    // We need to advance the ticks to something other than zero for most things to behave well.
    _tickSource->advance(Milliseconds{1});
}

void Simulation::teardown() {
    if (_monitor.joinable()) {
        _stopping.store(true);
        _monitor.join();
    }
    queue().stop();
    queue().clear();
}

void Simulation::_doRun() {}

void Simulation::run() {
    setup();

    LOGV2_INFO(7782100, "Starting simulation", "workload"_attr = workloadName());
    queue().start();

    _monitor = stdx::thread([this]() {
        // The monitoring interval of 200ms is definitely a bit of a magic number, and was
        // selected experimentally as the value that "just so happens" to produce lovely, smooth
        // graphs. A shorter interval produces very messy graphs, and would require
        // super-sampling in the processing scripts, and a longer interval misses many of the
        // probing events (this part is related to the default 100ms probing interval, and is
        // less magic).
        auto monitoringInterval = Milliseconds(200);
        while (!_stopping.load()) {
            if (queue().wait_for(monitoringInterval, EventQueue::WaitType::Observer)) {
                auto m = metrics();
                if (m) {
                    LOGV2_INFO(7782101,
                               "Workload metrics",
                               "metrics"_attr = m.value(),
                               "time"_attr = _tickSource->getTicks());
                }
            }
        }
    });

    _doRun();

    queue().prepareStop();
    teardown();
}

std::string Simulation::suiteName() const {
    return _suiteName;
}

std::string Simulation::workloadName() const {
    return _workloadName;
}

EventQueue& Simulation::queue() {
    return *_eventQueue;
}

ServiceContext* Simulation::svcCtx() {
    return _svcCtx.get();
}

Client* Simulation::client() {
    return _client.get();
}

SimulationRegistry& SimulationRegistry::get() {
    static SimulationRegistry registry;
    return registry;
}

void SimulationRegistry::runSelected() {
    for (auto&& workload : _workloads) {
        if (shouldRun(*workload)) {
            workload->run();
        }
    }
}

void SimulationRegistry::registerSimulation(std::unique_ptr<Simulation>&& simulation) {
    // Ensure we don't already have this workload registered.
    invariant(!_workloads.contains(simulation));
    _workloads.emplace(std::move(simulation));
}

void SimulationRegistry::list() const {
    std::string currentSuite = "";
    for (auto&& workload : _workloads) {
        if (currentSuite != workload->suiteName()) {
            currentSuite = workload->suiteName();
            std::cout << currentSuite << std::endl;
        }
        std::cout << " - " << workload->workloadName() << std::endl;
    }
}

}  // namespace mongo::workload_simulation
