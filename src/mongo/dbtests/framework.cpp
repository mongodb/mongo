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

#include "mongo/dbtests/framework.h"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/client/dbclient_base.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/dbtests/framework_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/sharding_state.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace dbtests {

int runDbTests(int argc, char** argv) {
    if (frameworkGlobalParams.suites.empty()) {
        LOGV2_ERROR(5733802, "The [suite] argument is required for dbtest and not specified here.");
        return static_cast<int>(ExitCode::fail);
    }

    frameworkGlobalParams.perfHist = 1;
    frameworkGlobalParams.seed = time(nullptr);
    frameworkGlobalParams.runsPerTest = 1;

    auto serviceContext = getGlobalServiceContext();

    registerShutdownTask([serviceContext] {
        // We drop the scope cache because leak sanitizer can't see across the thread we use for
        // proxying MozJS requests. Dropping the cache cleans up the memory and makes leak sanitizer
        // happy.
        ScriptEngine::dropScopeCache();

        // We may be shut down before we have a global storage engine.
        if (!serviceContext->getStorageEngine())
            return;

        shutdownGlobalStorageEngineCleanly(serviceContext);
    });

    ThreadClient tc("testsuite", serviceContext->getService());

    // DBTests run as if in the database, so allow them to create direct clients.
    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    srand((unsigned)frameworkGlobalParams.seed);  // NOLINT

    // Set up the periodic runner for background job execution, which is required by the storage
    // engine to be running beforehand.
    auto runner = makePeriodicRunner(serviceContext);
    serviceContext->setPeriodicRunner(std::move(runner));

    {
        auto initializeStorageEngineOpCtx = serviceContext->makeOperationContext(&cc());
        shard_role_details::setRecoveryUnit(initializeStorageEngineOpCtx.get(),
                                            std::make_unique<RecoveryUnitNoop>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

        initializeStorageEngine(initializeStorageEngineOpCtx.get(), StorageEngineInitFlags{});
    }

    StorageControl::startStorageControls(serviceContext, true /*forTestOnly*/);
    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());
    IndexBuildsCoordinator::set(serviceContext, std::make_unique<IndexBuildsCoordinatorMongod>());
    auto registry = std::make_unique<OpObserverRegistry>();
    serviceContext->setOpObserver(std::move(registry));
    ShardingState::create(serviceContext);
    CollectionShardingStateFactory::set(
        serviceContext, std::make_unique<CollectionShardingStateFactoryShard>(serviceContext));

    int ret = unittest::Suite::run(frameworkGlobalParams.suites,
                                   frameworkGlobalParams.filter,
                                   "",
                                   frameworkGlobalParams.runsPerTest);

    // So everything shuts down cleanly
    CollectionShardingStateFactory::clear(serviceContext);
    exitCleanly((ExitCode)ret);
    return ret;
}

}  // namespace dbtests
}  // namespace mongo
