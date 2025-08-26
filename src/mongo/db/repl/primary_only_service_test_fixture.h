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

MONGO_MOD_PUB extern FailPoint primaryOnlyServiceTestStepUpWaitForRebuildComplete;

class MONGO_MOD_OPEN PrimaryOnlyServiceMongoDTest : service_context_test::WithSetupTransportLayer,
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
