// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/throughput_probing_gen.h"
#include "mongo/tools/workload_simulation/simulation.h"
#include "mongo/tools/workload_simulation/throughput_probing/throughput_probing_simulator.h"
#include "mongo/tools/workload_simulation/throughput_probing/ticketed_workload_driver.h"
#include "mongo/tools/workload_simulation/workload_characteristics.h"
#include "mongo/util/duration.h"
#include "mongo/util/stacktrace.h"

namespace mongo::workload_simulation {
namespace {

/**
 * - Offered load exceeds optimal
 * - Optimal load and throughput are moderate
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, OverloadBasic) {
    RWPair optimalConcurrency{10, 10};
    RWPair throughputAtOptimalConcurrency{5'000, 5'000};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        optimalConcurrency, throughputAtOptimalConcurrency);

    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)), 128, 128);
    run(Seconds{20});
}

/**
 * - Offered load exceeds optimal
 * - Optimal load is moderate
 * - Throughput at optimal load is quite low
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, OverloadLowThroughput) {
    RWPair optimalConcurrency{10, 10};
    RWPair throughputAtOptimalConcurrency{100, 100};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        optimalConcurrency, throughputAtOptimalConcurrency);

    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)), 128, 128);
    run(Seconds{20});
}

/**
 * - Offered load exceeds optimal
 * - Optimal load is high
 * - Throughput at optimal load is high
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, OverloadHighConcurrency) {
    RWPair optimalConcurrency{100, 100};
    RWPair throughputAtOptimalConcurrency{50'000, 50'000};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        optimalConcurrency, throughputAtOptimalConcurrency);

    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)), 128, 128);
    run(Seconds{35});
}

/**
 * - Offered load exceeds optimal
 * - Two periods
 *   - For first, optimal load and throughput are moderate
 *   - For second, optimal load and throughput are high
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, OverloadAdjustsUpToDifferentWorkload) {
    RWPair initialOptimalConcurrency{10, 10};
    RWPair initialThroughputAtOptimalConcurrency{5'000, 5'000};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        initialOptimalConcurrency, initialThroughputAtOptimalConcurrency);
    auto characteristicsPtr = characteristics.get();

    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)), 128, 128);
    run(Seconds{20});

    // Change the workload so optimal is significantly higher than before.
    RWPair updatedOptimalConcurrency{100, 100};
    RWPair updatedThroughputAtOptimalConcurrency{50'000, 50'000};
    characteristicsPtr->reset(updatedOptimalConcurrency, updatedThroughputAtOptimalConcurrency);

    run(Seconds{35});
}

/**
 * - Offered load exceeds optimal
 * - Two periods
 *   - For first, optimal load and throughput are moderately high
 *   - For second, optimal load and throughput are half that in the first period
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, OverloadDoesNotAdjustDownToDifferentWorkload) {
    RWPair initialOptimalConcurrency{20, 20};
    RWPair initialThroughputAtOptimalConcurrency{20'000, 20'000};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        initialOptimalConcurrency, initialThroughputAtOptimalConcurrency);
    auto characteristicsPtr = characteristics.get();

    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)), 128, 128);
    run(Seconds{20});

    // Change the workload so optimal is half what it was before.
    RWPair updatedOptimalConcurrency{10, 10};
    RWPair updatedThroughputAtOptimalConcurrency{10'000, 10'000};
    characteristicsPtr->reset(updatedOptimalConcurrency, updatedThroughputAtOptimalConcurrency);

    run(Seconds{20});
}

/**
 * - Offered load does not exceed optimal
 * - Optimal load and throughput are moderate
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, StableBasic) {
    RWPair optimalConcurrency{20, 20};
    RWPair throughputAtOptimalConcurrency{5'000, 5'000};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        optimalConcurrency, throughputAtOptimalConcurrency);

    RWPair providedLoad{10, 10};
    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)),
          providedLoad.read,
          providedLoad.write);
    run(Seconds{20});
}

/**
 * - Offered load does not exceed optimal at any point
 * - Optimal load and throughput are moderate
 * - Two periods
 *   - For first, offered load is low
 *   - For second, offered load increases by 50%
 * - Standard monitoring interval
 */
SIMULATION(ThroughputProbing, StableAdjustsUpToProvidedLoad) {
    RWPair optimalConcurrency{20, 20};
    RWPair throughputAtOptimalConcurrency{5'000, 5'000};
    auto characteristics = std::make_unique<ParabolicWorkloadCharacteristics>(
        optimalConcurrency, throughputAtOptimalConcurrency);

    RWPair initialProvidedLoad{10, 10};
    start(std::make_unique<TicketedWorkloadDriver>(queue(), std::move(characteristics)),
          initialProvidedLoad.read,
          initialProvidedLoad.write);
    run(Seconds{20});

    // Increase offered load by 50%, but keep it under optimal level.
    RWPair higherProvidedLoad{15, 15};
    resize(higherProvidedLoad.read, higherProvidedLoad.write);
    run(Seconds(20));
}

}  // namespace
}  // namespace mongo::workload_simulation
