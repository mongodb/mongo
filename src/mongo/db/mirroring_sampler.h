// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/platform/random.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <vector>

namespace mongo {

/**
 * Populates a random subset of eligible secondaries (using `HelloResponse`
 * and a ratio) for mirroring. An empty subset for eligible secondaries indicates
 * no mirroring is necessary/possible.
 */
class MirroringSampler final {
public:
    using RandomFunc = std::function<int()>;

    static int threadSafeRandom() {
        static thread_local PseudoRandom random{SecureRandom{}.nextInt64()};
        return random.nextInt32(defaultRandomMax());
    }

    static RandomFunc defaultRandomFunc() {
        return threadSafeRandom;
    }

    static constexpr int defaultRandomMax() {
        return RAND_MAX;
    };

    /**
     * Sampling parameters for mirroring commands to eligible secondaries.
     *
     * Note that the value member is a raw integer and thus can be normalized to fit different
     * interpretations of ratio.
     */
    struct SamplingParameters {
        explicit SamplingParameters(double generalRatio,
                                    double targetedRatio,
                                    int rndMax,
                                    int rndValue);

        /**
         * Construct with a value from rnd().
         */
        explicit SamplingParameters(double generalRatio,
                                    double targetedRatio,
                                    int rndMax,
                                    RandomFunc rnd);

        /**
         * Construct with a value from defaultRandomFunc().
         */
        explicit SamplingParameters(const double generalRatio, double targetedRatio)
            : SamplingParameters(
                  generalRatio, targetedRatio, defaultRandomMax(), defaultRandomFunc()) {}

        const double generalRatio;
        const double targetedRatio;

        const int max;
        const int value;
    };

    struct MirroringMode {
        MirroringMode() = default;
        MirroringMode(bool general, bool targeted)
            : generalEnabled(general), targetedEnabled(targeted) {}

        bool shouldMirror() {
            return generalEnabled || targetedEnabled;
        }

        bool generalEnabled = false;
        bool targetedEnabled = false;
    };

    /**
     * Use the given imr and params to determine if we should attempt to sample.
     */
    MirroringMode getMirrorMode(const std::shared_ptr<const repl::HelloResponse>& imr,
                                const SamplingParameters& params) const;

    /**
     * Return all eligible hosts from a HelloResponse that we should mirror to.
     */
    std::vector<HostAndPort> getRawMirroringTargetsForGeneralMode(
        const std::shared_ptr<const repl::HelloResponse>& helloResponse);

    /**
     * Approximate use of the MirroringSampler for testing.
     *
     * In practice, we call constituent functions in sequence to pessimistically spare work.
     */
    static std::vector<HostAndPort> getGeneralMirroringTargets(
        const std::shared_ptr<const repl::HelloResponse>& helloResponse,
        double generalRatio,
        double targetedRatio,
        RandomFunc rnd = defaultRandomFunc(),
        int rndMax = defaultRandomMax());
};

}  // namespace mongo
