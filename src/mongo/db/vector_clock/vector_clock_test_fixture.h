/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
