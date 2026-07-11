// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo {

namespace executor {
class TaskExecutor;
}

class OperationContext;

class OpObserverRegistry;
class ServiceContext;

namespace repl {

class PrimaryOnlyService;
class PrimaryOnlyServiceRegistry;

[[MONGO_MOD_PUBLIC]] extern FailPoint primaryOnlyServiceTestStepUpWaitForRebuildComplete;

class [[MONGO_MOD_OPEN]] PrimaryOnlyServiceMongoDTest
    : service_context_test::WithSetupTransportLayer,
      public ServiceContextMongoDTest {
public:
    void setUp() override;
    void tearDown() override;

protected:
    PrimaryOnlyServiceMongoDTest() : ServiceContextMongoDTest(Options{}) {}
    explicit PrimaryOnlyServiceMongoDTest(Options options)
        : ServiceContextMongoDTest(std::move(options)) {}

    void startup(OperationContext* opCtx);
    void shutdown();

    void stepUp(OperationContext* opCtx);
    void stepDown();

    virtual std::unique_ptr<repl::PrimaryOnlyService> makeService(
        ServiceContext* serviceContext) = 0;

    virtual std::unique_ptr<repl::ReplicationCoordinator> makeReplicationCoordinator();

    /**
     * Used to add your own op observer to the op observer registry during setUp prior to running
     * your tests.
     */
    virtual void setUpOpObserverRegistry(OpObserverRegistry* opObserverRegistry) {};

    /**
     * Used in order to set persistent data (such as state doc on disk) during setUp prior to
     * running your tests.
     */
    virtual void setUpPersistence(OperationContext* opCtx) {};

    virtual void shutdownHook();

    OpObserverRegistry* _opObserverRegistry = nullptr;
    repl::PrimaryOnlyServiceRegistry* _registry = nullptr;
    repl::PrimaryOnlyService* _service = nullptr;
    long long _term = 0;
};

void stepUp(OperationContext* opCtx,
            ServiceContext* serviceCtx,
            repl::PrimaryOnlyServiceRegistry* registry,
            long long& term);

void stepDown(ServiceContext* serviceCtx, repl::PrimaryOnlyServiceRegistry* registry);

}  // namespace repl
}  // namespace mongo
