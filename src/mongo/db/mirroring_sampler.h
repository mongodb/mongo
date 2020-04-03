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

#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "mongo/db/repl/is_master_response.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * Populates a random subset of eligible secondaries (using `IsMasterResponse`
 * and a ratio) for mirroring. An empty subset for eligible secondaries indicates
 * no mirroring is necessary/possible.
 */
class MirroringSampler final {
public:
    using RandomFunc = std::function<int()>;

    static RandomFunc defaultRandomFunc() {
        return std::rand;
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
        explicit SamplingParameters(const double ratio, const int rndMax, const int rndValue);

        /**
         * Construct with a value from rnd().
         */
        explicit SamplingParameters(const double ratio, const int rndMax, RandomFunc rnd);

        /**
         * Construct with a value from defaultRandomFunc().
         */
        explicit SamplingParameters(const double ratio)
            : SamplingParameters(ratio, defaultRandomMax(), defaultRandomFunc()) {}

        const double ratio;

        const int max;
        const int value;
    };

    /**
     * Use the given imr and params to determine if we should attempt to sample.
     */
    bool shouldSample(const std::shared_ptr<const repl::IsMasterResponse>& imr,
                      const SamplingParameters& params) const noexcept;

    /**
     * Return all eligible hosts from an IsMasterResponse that we should mirror to.
     */
    std::vector<HostAndPort> getRawMirroringTargets(
        const std::shared_ptr<const repl::IsMasterResponse>& isMaster) noexcept;

    /**
     * Approximate use of the MirroringSampler for testing.
     *
     * In practice, we call constituent functions in sequence to pessimistically spare work.
     */
    static std::vector<HostAndPort> getMirroringTargets(
        const std::shared_ptr<const repl::IsMasterResponse>& isMaster,
        const double ratio,
        RandomFunc rnd = defaultRandomFunc(),
        const int rndMax = defaultRandomMax()) noexcept;
};

}  // namespace mongo
