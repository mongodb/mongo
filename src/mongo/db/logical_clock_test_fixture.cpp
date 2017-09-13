/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/logical_clock_test_fixture.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

LogicalClockTestFixture::LogicalClockTestFixture() = default;

LogicalClockTestFixture::~LogicalClockTestFixture() = default;

void LogicalClockTestFixture::setUp() {
    ShardingMongodTestFixture::setUp();

    auto service = getServiceContext();

    auto logicalClock = stdx::make_unique<LogicalClock>(service);
    LogicalClock::set(service, std::move(logicalClock));
    _clock = LogicalClock::get(service);

    service->setFastClockSource(stdx::make_unique<SharedClockSourceAdapter>(_mockClockSource));
    service->setPreciseClockSource(stdx::make_unique<SharedClockSourceAdapter>(_mockClockSource));

    _dbDirectClient = stdx::make_unique<DBDirectClient>(operationContext());

    // Set master to false (set to true in ShardingMongodTestFixture::setUp()) so follower mode can
    // be toggled meaningfully. Default follower mode to primary, so writes can be accepted.
    replicationCoordinator()->setMaster(false);
    ASSERT_OK(replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));
}

void LogicalClockTestFixture::tearDown() {
    _dbDirectClient.reset();
    ShardingMongodTestFixture::tearDown();
}

LogicalClock* LogicalClockTestFixture::resetClock() {
    auto service = getServiceContext();
    auto logicalClock = stdx::make_unique<LogicalClock>(service);

    LogicalClock::set(service, std::move(logicalClock));
    _clock = LogicalClock::get(service);

    return _clock;
}

LogicalClock* LogicalClockTestFixture::getClock() const {
    return _clock;
}

ClockSourceMock* LogicalClockTestFixture::getMockClockSource() const {
    return _mockClockSource.get();
}

void LogicalClockTestFixture::setMockClockSourceTime(Date_t time) const {
    _mockClockSource->reset(time);
}

Date_t LogicalClockTestFixture::getMockClockSourceTime() const {
    return _mockClockSource->now();
}

DBDirectClient* LogicalClockTestFixture::getDBClient() const {
    return _dbDirectClient.get();
}

}  // namespace mongo
