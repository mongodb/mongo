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

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"

using namespace mongo;
using namespace mongo::repl;

class TenantMigrationRecipientServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        {
            auto opCtx = cc().makeOperationContext();
            auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
            ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::setOplogCollectionName(serviceContext);
            repl::createOplog(opCtx.get());
            // Set up OpObserver so that repl::logOp() will store the oplog entry's optime in
            // ReplClientInfo.
            OpObserverRegistry* opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
            opObserverRegistry->addObserver(
                std::make_unique<PrimaryOnlyServiceOpObserver>(serviceContext));

            _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
            std::unique_ptr<TenantMigrationRecipientService> service =
                std::make_unique<TenantMigrationRecipientService>(getServiceContext());
            _registry->registerService(std::move(service));
            _registry->onStartup(opCtx.get());
        }
        stepUp();

        _service = _registry->lookupServiceByName(
            TenantMigrationRecipientService::kTenantMigrationRecipientServiceName);
        ASSERT(_service);
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        _registry->onShutdown();
        _service = nullptr;

        ServiceContextMongoDTest::tearDown();
    }

    void stepDown() {
        ASSERT_OK(ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(MemberState::RS_SECONDARY));
        _registry->onStepDown();
    }

    void stepUp() {
        auto opCtx = cc().makeOperationContext();
        auto replCoord = ReplicationCoordinator::get(getServiceContext());

        // Advance term
        _term++;

        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            OpTimeAndWallTime(OpTime(Timestamp(1, 1), _term), Date_t()));

        _registry->onStepUpComplete(opCtx.get(), _term);
    }

protected:
    PrimaryOnlyServiceRegistry* _registry;
    PrimaryOnlyService* _service;
    long long _term = 0;
};


TEST_F(TenantMigrationRecipientServiceTest, BasicTenantMigrationRecipientServiceInstanceCreation) {
    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        "DonorHost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Wait for task completion success.
    ASSERT_OK(instance->getCompletionFuture().getNoThrow());
}


TEST_F(TenantMigrationRecipientServiceTest, InstanceReportsErrorOnFailureWhilePersisitingStateDoc) {
    FailPointEnableBlock failPoint("failWhilePersistingTenantMigrationRecipientInstanceStateDoc");

    const UUID migrationUUID = UUID::gen();

    TenantMigrationRecipientDocument TenantMigrationRecipientInstance(
        migrationUUID,
        "DonorHost:12345",
        "tenantA",
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Create and start the instance.
    auto instance = TenantMigrationRecipientService::Instance::getOrCreate(
        _service, TenantMigrationRecipientInstance.toBSON());
    ASSERT(instance.get());
    ASSERT_EQ(migrationUUID, instance->getMigrationUUID());

    // Should be able to see the instance task failure error.
    auto status = instance->getCompletionFuture().getNoThrow();
    ASSERT_EQ(ErrorCodes::NotWritablePrimary, status.code());
}