// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/simulation.h"

#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/tools/workload_simulation/simulator_options.h"
#include "mongo/util/pcre.h"

#include <string_view>

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

}  // namespace

Simulation::Simulation(std::string_view suiteName, std::string_view workloadName)
    : _suiteName(std::string{suiteName}), _workloadName(std::string{workloadName}) {}

Simulation::~Simulation() {}

void Simulation::setup() {
    _svcCtx =
        ServiceContext::make(nullptr, nullptr, std::make_unique<TickSourceMock<Nanoseconds>>());
    _client = _svcCtx->getService()->makeClient(suiteName());

    _tickSource = checked_cast<TickSourceMock<Nanoseconds>*>(_svcCtx->getTickSource());
    _eventQueue = std::make_unique<EventQueue>(*_tickSource, [this]() {
        // Make the queue wait to process events until it has the same number of events queued as we
        // have busy actors. Otherwise it may advance the clock before we have a chance to queue our
        // events.
        return actorCount();
    });

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
