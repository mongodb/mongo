/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/repl/primary_only_service_test_fixture.h"

#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace repl {

void PrimaryOnlyServiceMongoDTest::setUp() {
    ServiceContextMongoDTest::setUp();

    auto serviceContext = getServiceContext();
    WaitForMajorityService::get(serviceContext).startup(serviceContext);

    {
        auto opCtx = makeOperationContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        repl::createOplog(opCtx.get());

        // This method was added in order to write data on disk during setUp which is called
        // during a test case construction.
        setUpPersistence(opCtx.get());

        // Set up OpObserverImpl so that repl::logOp() will store the oplog entry's optime in
        // ReplClientInfo.
        _opObserverRegistry = dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
        invariant(_opObserverRegistry);

        _opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
        _opObserverRegistry->addObserver(
            std::make_unique<repl::PrimaryOnlyServiceOpObserver>(serviceContext));
        setUpOpObserverRegistry(_opObserverRegistry);

        _registry = repl::PrimaryOnlyServiceRegistry::get(serviceContext);
        auto service = makeService(serviceContext);
        auto serviceName = service->getServiceName();
        _registry->registerService(std::move(service));
        _service = _registry->lookupServiceByName(serviceName);

        startup(opCtx.get());
        stepUp(opCtx.get());
    }
}

void PrimaryOnlyServiceMongoDTest::tearDown() {
    // Ensure that even on test failures all failpoint state gets reset.
    globalFailPointRegistry().disableAllFailpoints();

    WaitForMajorityService::get(getServiceContext()).shutDown();

    shutdown();

    ServiceContextMongoDTest::tearDown();
}

void PrimaryOnlyServiceMongoDTest::startup(OperationContext* opCtx) {
    _registry->onStartup(opCtx);
}

void PrimaryOnlyServiceMongoDTest::shutdown() {
    _registry->onShutdown();
}

void PrimaryOnlyServiceMongoDTest::stepUp(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
    auto currOpTime = replCoord->getMyLastAppliedOpTime();

    // Advance the term and last applied opTime. We retain the timestamp component of the current
    // last applied opTime to avoid log messages from ReplClientInfo::setLastOpToSystemLastOpTime()
    // about the opTime having moved backwards.
    ++_term;
    auto newOpTime = OpTime{currOpTime.getTimestamp(), _term};

    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    ASSERT_OK(replCoord->updateTerm(opCtx, _term));
    replCoord->setMyLastAppliedOpTimeAndWallTime({newOpTime, {}});

    _registry->onStepUpComplete(opCtx, _term);
}

void PrimaryOnlyServiceMongoDTest::stepDown() {
    ASSERT_OK(ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(MemberState::RS_SECONDARY));
    _registry->onStepDown();
}

}  // namespace repl
}  // namespace mongo
