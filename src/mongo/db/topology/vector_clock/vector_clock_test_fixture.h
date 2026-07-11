// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/logical_time.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/time_support.h"

#include <memory>

namespace mongo {

class ClockSourceMock;
class DBDirectClient;
class LogicalTime;
class VectorClock;
class VectorClockMutable;

/**
 * A test fixture that installs a VectorClock instance with a TimeProofService onto a service
 * context, in addition to the mock storage engine, network, and OpObserver provided by
 * ShardingMongoDTestFixture.
 */
class VectorClockTestFixture : public ShardingMongoDTestFixture {
protected:
    VectorClockTestFixture();
    ~VectorClockTestFixture() override;

    /**
     * Sets up this fixture as the primary node in a shard server replica set with a VectorClock
     * (with a TimeProofService), storage engine, DBClient, OpObserver, and a mocked clock source.
     */
    void setUp() override;

    void tearDown() override;

    VectorClockMutable* resetClock();

    void advanceClusterTime(LogicalTime newTime);

    VectorClock* getClock() const;

    LogicalTime getClusterTime() const;

    ClockSourceMock* getMockClockSource();

    void setMockClockSourceTime(Date_t time);

    Date_t getMockClockSourceTime();

    DBDirectClient* getDBClient() const;

    void setupOpObservers() override;

private:
    VectorClock* _clock;
    ClockSourceMock _mockClockSource;
    std::unique_ptr<DBDirectClient> _dbDirectClient;
};

}  // namespace mongo
