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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/mirroring_sampler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

DEATH_TEST(MirroringSamplerTest, ValidateNegativeRatio, "invariant") {
    auto dummyIsMaster = std::make_shared<mongo::repl::IsMasterResponse>();
    MirroringSampler::getMirroringTargets(dummyIsMaster, -1);
}

DEATH_TEST(MirroringSamplerTest, ValidateLargeRatio, "invariant") {
    auto dummyIsMaster = std::make_shared<mongo::repl::IsMasterResponse>();
    MirroringSampler::getMirroringTargets(dummyIsMaster, 1.1);
}

TEST(MirroringSamplerTest, ValidateMissingIsMaster) {
    auto targets = MirroringSampler::getMirroringTargets(nullptr, 1);
    ASSERT_EQ(targets.size(), 0);
}

TEST(MirroringSamplerTest, ValidateHostIsPrimary) {
    auto isMaster = std::make_shared<mongo::repl::IsMasterResponse>();
    isMaster->setIsMaster(false);

    auto targets = MirroringSampler::getMirroringTargets(isMaster, 1);
    ASSERT_EQ(targets.size(), 0);
}

TEST(MirroringSamplerTest, NoEligibleSecondary) {
    auto isMaster = std::make_shared<mongo::repl::IsMasterResponse>();
    isMaster->setIsMaster(true);
    isMaster->setIsSecondary(false);
    isMaster->addHost(mongo::HostAndPort("primary", 12345));
    isMaster->setPrimary(isMaster->getHosts()[0]);
    isMaster->setMe(isMaster->getPrimary());

    auto targets = MirroringSampler::getMirroringTargets(isMaster, 1.0);
    ASSERT_EQ(targets.size(), 0);
}

class MirroringSamplerFixture : public unittest::Test {
public:
    void init(size_t secondariesCount) {
        _isMaster = std::make_shared<mongo::repl::IsMasterResponse>();
        _isMaster->setIsMaster(true);
        _isMaster->setIsSecondary(false);
        for (size_t i = 0; i < secondariesCount + 1; i++) {
            std::string hostName = "node-" + std::to_string(i);
            _isMaster->addHost(mongo::HostAndPort(hostName, 12345));
        }

        _isMaster->setPrimary(_isMaster->getHosts()[0]);
        _isMaster->setMe(_isMaster->getPrimary());

        _hitCounts.clear();
        for (size_t i = 1; i < secondariesCount + 1; i++) {
            _hitCounts[_isMaster->getHosts()[i].toString()] = 0;
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
        for (auto pair : _hitCounts) {
            _hitCounts[pair.first] = 0;
        }
    }

    void populteHitCounts(std::vector<HostAndPort>& targets) {
        for (auto host : targets) {
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
        for (auto pair : _hitCounts) {
            standardDeviation += std::pow(pair.second - mean, 2);
        }

        return std::sqrt(standardDeviation / _hitCounts.size());
    }

    auto getIsMaster() const {
        return _isMaster;
    }

    const size_t repeats = 100000;

private:
    std::shared_ptr<repl::IsMasterResponse> _isMaster;

    double _pseudoRandomSeed;

    stdx::unordered_map<std::string, size_t> _hitCounts;
};

TEST_F(MirroringSamplerFixture, SamplerFunction) {

    std::vector<size_t> secondariesCount = {1, 2, 3, 4, 5, 6, 7};
    std::vector<double> ratios = {
        0.01, 0.02, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90};
    for (auto secondaryQ : secondariesCount) {
        // Set number of secondaries
        init(secondaryQ);
        auto isMaster = getIsMaster();

        for (auto ratio : ratios) {
            resetPseudoRandomSeed();
            resetHitCounts();

            auto pseudoRandomGen = [&]() -> int { return this->nextPseudoRandom(); };

            for (size_t i = 0; i < repeats; i++) {
                auto targets =
                    MirroringSampler::getMirroringTargets(isMaster, ratio, pseudoRandomGen);
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

TEST_F(MirroringSamplerFixture, MirrorAll) {
    std::vector<size_t> secondariesCount = {1, 2, 3, 4, 5, 6, 7};
    for (auto secondaryQ : secondariesCount) {
        // Set number of secondaries
        init(secondaryQ);
        auto isMaster = getIsMaster();

        for (size_t i = 0; i < repeats; i++) {
            auto targets = MirroringSampler::getMirroringTargets(isMaster, 1.0);
            populteHitCounts(targets);
        }

        const double observedMirroredCmds = getHitCountsSum();
        const double expectedMirroredCmds = repeats * secondaryQ;
        ASSERT_EQ(getHitCountsSTD(), 0);
        ASSERT_EQ(observedMirroredCmds, expectedMirroredCmds);
    }
}

}  // namespace mongo
