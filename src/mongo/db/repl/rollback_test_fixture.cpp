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

#include "mongo/db/repl/rollback_test_fixture.h"

#include <string>

#include "mongo/db/client.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Creates ReplSettings for ReplicationCoordinatorRollbackMock.
 */
ReplSettings createReplSettings() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

}  // namespace

void RollbackTest::setUp() {
    _serviceContextMongoDTest.setUp();
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    _replicationProcess = stdx::make_unique<ReplicationProcess>(
        &_storageInterface, stdx::make_unique<ReplicationConsistencyMarkersMock>());
    _dropPendingCollectionReaper = new DropPendingCollectionReaper(&_storageInterface);
    DropPendingCollectionReaper::set(
        serviceContext, std::unique_ptr<DropPendingCollectionReaper>(_dropPendingCollectionReaper));
    _coordinator = new ReplicationCoordinatorRollbackMock(serviceContext);
    ReplicationCoordinator::set(serviceContext,
                                std::unique_ptr<ReplicationCoordinator>(_coordinator));
    setOplogCollectionName();

    SessionCatalog::create(serviceContext);

    _opCtx = cc().makeOperationContext();
    _replicationProcess->getConsistencyMarkers()->setAppliedThrough(_opCtx.get(), OpTime{});
    _replicationProcess->getConsistencyMarkers()->setMinValid(_opCtx.get(), OpTime{});
    _replicationProcess->initializeRollbackID(_opCtx.get()).transitional_ignore();
}

void RollbackTest::tearDown() {
    _coordinator = nullptr;
    _opCtx.reset();

    SessionCatalog::reset_forTest(_serviceContextMongoDTest.getServiceContext());

    // We cannot unset the global replication coordinator because ServiceContextMongoD::tearDown()
    // calls dropAllDatabasesExceptLocal() which requires the replication coordinator to clear all
    // snapshots.
    _serviceContextMongoDTest.tearDown();

    // ServiceContextMongoD::tearDown() does not destroy service context so it is okay
    // to access the service context after tearDown().
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    _replicationProcess.reset();
    ReplicationCoordinator::set(serviceContext, {});
}

RollbackTest::ReplicationCoordinatorRollbackMock::ReplicationCoordinatorRollbackMock(
    ServiceContext* service)
    : ReplicationCoordinatorMock(service, createReplSettings()) {}

void RollbackTest::ReplicationCoordinatorRollbackMock::resetLastOpTimesFromOplog(
    OperationContext* opCtx) {}

void RollbackTest::ReplicationCoordinatorRollbackMock::failSettingFollowerMode(
    const MemberState& transitionToFail, ErrorCodes::Error codeToFailWith) {
    _failSetFollowerModeOnThisMemberState = transitionToFail;
    _failSetFollowerModeWithThisCode = codeToFailWith;
}

Status RollbackTest::ReplicationCoordinatorRollbackMock::setFollowerMode(
    const MemberState& newState) {
    if (newState == _failSetFollowerModeOnThisMemberState) {
        return Status(_failSetFollowerModeWithThisCode,
                      str::stream()
                          << "ReplicationCoordinatorRollbackMock set to fail on setting state to "
                          << _failSetFollowerModeOnThisMemberState.toString());
    }
    return ReplicationCoordinatorMock::setFollowerMode(newState);
}

}  // namespace repl
}  // namespace mongo
