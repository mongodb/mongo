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

#include "mongo/platform/basic.h"

#include "mongo/db/vector_clock_test_fixture.h"

#include <memory>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

VectorClockTestFixture::VectorClockTestFixture()
    : ShardingMongodTestFixture(Options{}.useMockClock(true), false /* setUpMajorityReads */) {}

VectorClockTestFixture::~VectorClockTestFixture() = default;

void VectorClockTestFixture::setUp() {
    ShardingMongodTestFixture::setUp();

    auto service = getServiceContext();

    _clock = VectorClock::get(service);

    _dbDirectClient = std::make_unique<DBDirectClient>(operationContext());

    ASSERT_OK(replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));
}

void VectorClockTestFixture::tearDown() {
    _dbDirectClient.reset();
    ShardingMongodTestFixture::tearDown();
}

VectorClockMutable* VectorClockTestFixture::resetClock() {
    auto service = getServiceContext();
    VectorClock::get(service)->resetVectorClock_forTest();
    return VectorClockMutable::get(service);
}

void VectorClockTestFixture::advanceClusterTime(LogicalTime newTime) {
    VectorClock::get(getServiceContext())->advanceClusterTime_forTest(newTime);
}

VectorClock* VectorClockTestFixture::getClock() const {
    return _clock;
}

LogicalTime VectorClockTestFixture::getClusterTime() const {
    auto now = getClock()->getTime();
    return now.clusterTime();
}

ClockSourceMock* VectorClockTestFixture::getMockClockSource() {
    return &_mockClockSource;
}

void VectorClockTestFixture::setMockClockSourceTime(Date_t time) {
    _mockClockSource.reset(time);
}

Date_t VectorClockTestFixture::getMockClockSourceTime() {
    return _mockClockSource.now();
}

DBDirectClient* VectorClockTestFixture::getDBClient() const {
    return _dbDirectClient.get();
}

void VectorClockTestFixture::setupOpObservers() {
    auto opObserverRegistry =
        checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
    opObserverRegistry->addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
}

}  // namespace mongo
