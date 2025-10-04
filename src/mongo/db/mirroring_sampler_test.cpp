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

#include "mongo/db/mirroring_sampler.h"

#include "mongo/base/string_data.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cmath>
#include <numeric>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

namespace mongo {

DEATH_TEST(MirroringSamplerTest, ValidateNegativeGeneralRatio, "invariant") {
    auto dummyHello = std::make_shared<mongo::repl::HelloResponse>();
    MirroringSampler::getGeneralMirroringTargets(dummyHello, -1, 0);
}

DEATH_TEST(MirroringSamplerTest, ValidateLargeGeneralRatio, "invariant") {
    auto dummyHello = std::make_shared<mongo::repl::HelloResponse>();
    MirroringSampler::getGeneralMirroringTargets(dummyHello, 1.1, 0);
}

TEST(MirroringSamplerTest, ValidateMissingHello) {
    auto targets = MirroringSampler::getGeneralMirroringTargets(nullptr, 1, 1);
    ASSERT_EQ(targets.size(), 0);
}

DEATH_TEST(MirroringSamplerTest, ValidateNegativeTargetedRatio, "invariant") {
    MirroringSampler::SamplingParameters(0, -1);
}

DEATH_TEST(MirroringSamplerTest, ValidateLargeTargetedRatio, "invariant") {
    MirroringSampler::SamplingParameters(0, 1.1);
}

TEST(MirroringSamplerTest, ValidateHostIsPrimaryForGeneral) {
    auto hello = std::make_shared<mongo::repl::HelloResponse>();
    hello->setIsWritablePrimary(false);

    auto targets = MirroringSampler::getGeneralMirroringTargets(hello, 1, 0);
    ASSERT_EQ(targets.size(), 0);
}

TEST(MirroringSamplerTest, ValidateHostOKIfNotPrimaryForTargeted) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagTargetedMirrorReads",
                                                               true);

    auto hello = std::make_shared<mongo::repl::HelloResponse>();
    hello->setIsWritablePrimary(false);
    hello->setIsSecondary(true);
    hello->addHost(mongo::HostAndPort("node0", 12345));
    hello->addHost(mongo::HostAndPort("node1", 12345));
    hello->setMe(hello->getHosts()[0]);
    hello->setPrimary(hello->getHosts()[1]);

    auto sampler = MirroringSampler();
    auto params = MirroringSampler::SamplingParameters(0.1, 1);

    auto mode = sampler.getMirrorMode(hello, params);
    ASSERT(mode.targetedEnabled);
}

TEST(MirroringSamplerTest, NoEligibleSecondary) {
    auto hello = std::make_shared<mongo::repl::HelloResponse>();
    hello->setIsWritablePrimary(true);
    hello->setIsSecondary(false);
    hello->addHost(mongo::HostAndPort("primary", 12345));
    hello->setPrimary(hello->getHosts()[0]);
    hello->setMe(hello->getPrimary());

    auto targets = MirroringSampler::getGeneralMirroringTargets(hello, 1.0, 1.0);
    ASSERT_EQ(targets.size(), 0);
}

class MirroringSamplerFixture : public unittest::Test {
public:
    void init(bool isPrimary, size_t secondariesCount) {
        _hello = std::make_shared<mongo::repl::HelloResponse>();
        _hello->setIsWritablePrimary(isPrimary);
        _hello->setIsSecondary(!isPrimary);
        for (size_t i = 0; i < secondariesCount + 1; i++) {
            std::string hostName = "node-" + std::to_string(i);
            _hello->addHost(mongo::HostAndPort(hostName, 12345));
        }

        _hitCounts.clear();
        if (isPrimary) {
            _hello->setPrimary(_hello->getHosts()[0]);
            _hello->setMe(_hello->getPrimary());

            for (size_t i = 1; i < secondariesCount + 1; i++) {
                _hitCounts[_hello->getHosts()[i].toString()] = 0;
            }
        } else {
            // Choose some other node as primary
            _hello->setPrimary(_hello->getHosts()[secondariesCount - 1]);
            _hello->setMe(_hello->getHosts()[0]);

            for (size_t i = 0; i < secondariesCount; i++) {
                _hitCounts[_hello->getHosts()[i].toString()] = 0;
            }
        }
    }

    void resetPseudoRandomSeed() {
        _pseudoRandomSeed = 0.0;
    }

    int nextPseudoRandom() {
        const auto stepSize = static_cast<double>(RAND_MAX) / repeats;
        _pseudoRandomSeed += stepSize;
        return static_cast<int>(_pseudoRandomSeed) % RAND_MAX;
    }

    void resetHitCounts() {
        for (const auto& pair : _hitCounts) {
            _hitCounts[pair.first] = 0;
        }
    }

    void populteHitCounts(std::vector<HostAndPort>& targets) {
        for (const auto& host : targets) {
            auto it = _hitCounts.find(host.toString());
            invariant(it != _hitCounts.end());
            it->second++;
        }
    }

    size_t getHitCountsSum() {
        return std::accumulate(
            _hitCounts.begin(),
            _hitCounts.end(),
            0,
            [](int value, const std::pair<std::string, size_t>& p) { return value + p.second; });
    }

    double getHitCounsMean() {
        return static_cast<double>(getHitCountsSum()) / _hitCounts.size();
    }

    double getHitCountsSTD() {
        const auto mean = getHitCounsMean();
        double standardDeviation = 0.0;
        for (const auto& pair : _hitCounts) {
            standardDeviation += std::pow(pair.second - mean, 2);
        }

        return std::sqrt(standardDeviation / _hitCounts.size());
    }

    auto getHello() const {
        return _hello;
    }

    const size_t repeats = 100000;

private:
    std::shared_ptr<repl::HelloResponse> _hello;

    double _pseudoRandomSeed;

    stdx::unordered_map<std::string, size_t> _hitCounts;
};

TEST_F(MirroringSamplerFixture, SamplerFunctionGeneralMirror) {

    std::vector<size_t> secondariesCount = {1, 2, 3, 4, 5, 6, 7};
    std::vector<double> ratios = {
        0.01, 0.02, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90};
    for (auto secondaryQ : secondariesCount) {
        // Set number of secondaries
        init(true /* isPrimary */, secondaryQ);
        auto hello = getHello();

        for (auto ratio : ratios) {
            resetPseudoRandomSeed();
            resetHitCounts();

            auto pseudoRandomGen = [&]() -> int {
                return this->nextPseudoRandom();
            };

            for (size_t i = 0; i < repeats; i++) {
                auto targets =
                    MirroringSampler::getGeneralMirroringTargets(hello, ratio, 0, pseudoRandomGen);
                populteHitCounts(targets);
            }

            // The number of mirrored commands is at least 95% of the promise
            const double observedMirroredCmds = getHitCountsSum();
            const double expectedMirroredCmds = repeats * ratio * secondaryQ;
            ASSERT_GT(observedMirroredCmds / expectedMirroredCmds, 0.95);

            /**
             * Relative standard deviation (a metric for distribution of mirrored
             * commands among secondaries) must be less than 7%.
             */
            const auto relativeSTD = getHitCountsSTD() / getHitCounsMean();
            ASSERT_LT(relativeSTD, 0.07);
        }
    }
}

TEST_F(MirroringSamplerFixture, GeneralMirrorAll) {
    std::vector<size_t> secondariesCount = {1, 2, 3, 4, 5, 6, 7};
    for (auto secondaryQ : secondariesCount) {
        // Set number of secondaries
        init(true /* isPrimary */, secondaryQ);
        auto hello = getHello();

        for (size_t i = 0; i < repeats; i++) {
            auto targets = MirroringSampler::getGeneralMirroringTargets(hello, 1.0, 0);
            populteHitCounts(targets);
        }

        const double observedMirroredCmds = getHitCountsSum();
        const double expectedMirroredCmds = repeats * secondaryQ;
        ASSERT_EQ(getHitCountsSTD(), 0);
        ASSERT_EQ(observedMirroredCmds, expectedMirroredCmds);
    }
}

}  // namespace mongo
