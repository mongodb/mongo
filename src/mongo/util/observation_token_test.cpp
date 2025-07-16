/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observation_token.h"

#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ObservationTokenTest : public unittest::Test {
public:
    using LockStats = observable_mutex_details::LockStats;
    using ObservationToken = observable_mutex_details::ObservationToken;

    ObservationTokenTest()
        : obsToken(ObservationToken([this](LockStats& stats) {
              auto append = [](auto& agg, auto& sample) {
                  agg.total += sample.total;
                  agg.contentions += sample.contentions;
                  agg.waitCycles += sample.waitCycles;
              };
              append(stats.exclusiveAcquisitions, _tokenStats.exclusiveAcquisitions);
              append(stats.sharedAcquisitions, _tokenStats.sharedAcquisitions);
          })) {};

    void setTokenStats(LockStats& newStats) {
        _tokenStats = newStats;
    }

    void verifyLockStats(const LockStats& current, const LockStats& expected) const {
        using AcquisitionStats = observable_mutex_details::AcquisitionStats<uint64_t>;

        auto transformAcqStats = [](const AcquisitionStats& stats) {
            return std::tie(stats.total, stats.contentions, stats.waitCycles);
        };
        auto transformLockStats = [transformAcqStats](const LockStats& stats) {
            return transformAcqStats(stats.exclusiveAcquisitions),
                   transformAcqStats(stats.sharedAcquisitions);
        };

        auto actualV = transformLockStats(current);
        auto expectedV = transformLockStats(expected);
        ASSERT_EQ(actualV, expectedV);
    }

    ObservationToken obsToken;

private:
    // The following mimics the metrics collected by an observable mutex.
    LockStats _tokenStats;
};

class InvalidObservationTokenTest : public ObservationTokenTest {
public:
    void setUp() override {
        ObservationTokenTest::setUp();
        obsToken.invalidate();
    }
};

const observable_mutex_details::LockStats kZeroStats = {{0, 0, 0}, {0, 0, 0}};

TEST_F(ObservationTokenTest, Validity) {
    ASSERT(obsToken.isValid());
}

TEST_F(ObservationTokenTest, ValidInitStats) {
    verifyLockStats(*obsToken.getStats(), kZeroStats);
}

TEST_F(ObservationTokenTest, ValidGetStats) {
    // Mimic a process updating the token's stats.
    LockStats newStats = {{1, 1, 1}, {1, 1, 1}};
    setTokenStats(newStats);

    // Check that retrieving the token's stats shows updated stats.
    auto fetchedTokenStats = obsToken.getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyLockStats(*fetchedTokenStats, newStats);
}

TEST_F(InvalidObservationTokenTest, Invalidity) {
    ASSERT_FALSE(obsToken.isValid());
}

TEST_F(InvalidObservationTokenTest, InvalidInitStats) {
    ASSERT_FALSE(obsToken.getStats());
}

TEST_F(ObservationTokenTest, ValidThenInvalid) {
    // Mimic a process updating the token's stats.
    LockStats firstUpdate = {{1, 1, 1}, {1, 1, 1}};
    setTokenStats(firstUpdate);

    // Check that retrieving the token's stats shows updated stats.
    auto fetchedTokenStats = obsToken.getStats();
    ASSERT_TRUE(fetchedTokenStats);
    verifyLockStats(*fetchedTokenStats, firstUpdate);

    // Invalidate the token.
    obsToken.invalidate();

    // Check that retrieving the token's stats returns an optional.
    ASSERT_FALSE(obsToken.getStats());
}

}  // namespace
}  // namespace mongo
