/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/platform/random.h"
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
        static StaticImmortal<synchronized_value<PseudoRandom>> random{
            PseudoRandom{SecureRandom{}.nextInt64()}};
        return (*random)->nextInt32(defaultRandomMax());
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
