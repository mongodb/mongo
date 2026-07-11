// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/vector_clock/vector_clock_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <memory>

namespace mongo {

VectorClockTestFixture::VectorClockTestFixture()
    : ShardingMongoDTestFixture(Options{}.useMockClock(true), false /* setUpMajorityReads */) {}

VectorClockTestFixture::~VectorClockTestFixture() = default;

void VectorClockTestFixture::setUp() {
    ShardingMongoDTestFixture::setUp();

    auto service = getServiceContext();

    _clock = VectorClock::get(service);

    _dbDirectClient = std::make_unique<DBDirectClient>(operationContext());

    ASSERT_OK(replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));
}

void VectorClockTestFixture::tearDown() {
    _dbDirectClient.reset();
    ShardingMongoDTestFixture::tearDown();
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
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
}

}  // namespace mongo
