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

#include "mongo/dbtests/framework.h"

#include <string>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_access_method_factory_impl.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/framework_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version.h"

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

    registerShutdownTask([] {
        // We drop the scope cache because leak sanitizer can't see across the
        // thread we use for proxying MozJS requests. Dropping the cache cleans up
        // the memory and makes leak sanitizer happy.
        ScriptEngine::dropScopeCache();

        // We may be shut down before we have a global storage
        // engine.
        if (!getGlobalServiceContext()->getStorageEngine())
            return;

        shutdownGlobalStorageEngineCleanly(getGlobalServiceContext());
    });

    Client::initThread("testsuite");

    auto globalServiceContext = getGlobalServiceContext();
    CollectionShardingStateFactory::set(
        globalServiceContext,
        std::make_unique<CollectionShardingStateFactoryShard>(globalServiceContext));


    // DBTests run as if in the database, so allow them to create direct clients.
    DBDirectClientFactory::get(globalServiceContext)
        .registerImplementation([](OperationContext* opCtx) {
            return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
        });

    srand((unsigned)frameworkGlobalParams.seed);

    // Set up the periodic runner for background job execution, which is required by the storage
    // engine to be running beforehand.
    auto runner = makePeriodicRunner(globalServiceContext);
    globalServiceContext->setPeriodicRunner(std::move(runner));

    {
        auto opCtx = globalServiceContext->makeOperationContext(&cc());
        initializeStorageEngine(opCtx.get(), StorageEngineInitFlags{});
    }

    StorageControl::startStorageControls(globalServiceContext, true /*forTestOnly*/);
    DatabaseHolder::set(globalServiceContext, std::make_unique<DatabaseHolderImpl>());
    IndexAccessMethodFactory::set(globalServiceContext,
                                  std::make_unique<IndexAccessMethodFactoryImpl>());
    Collection::Factory::set(globalServiceContext, std::make_unique<CollectionImpl::FactoryImpl>());
    IndexBuildsCoordinator::set(globalServiceContext,
                                std::make_unique<IndexBuildsCoordinatorMongod>());
    auto registry = std::make_unique<OpObserverRegistry>();
    globalServiceContext->setOpObserver(std::move(registry));

    int ret = unittest::Suite::run(frameworkGlobalParams.suites,
                                   frameworkGlobalParams.filter,
                                   "",
                                   frameworkGlobalParams.runsPerTest);

    // So everything shuts down cleanly
    CollectionShardingStateFactory::clear(globalServiceContext);
    exitCleanly((ExitCode)ret);
    return ret;
}

}  // namespace dbtests

}  // namespace mongo
