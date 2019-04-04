/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kDefault

#include "merizo/platform/basic.h"

#include "merizo/dbtests/framework.h"

#include <string>

#include "merizo/base/checked_cast.h"
#include "merizo/base/status.h"
#include "merizo/db/catalog/database_holder_impl.h"
#include "merizo/db/catalog/uuid_catalog.h"
#include "merizo/db/client.h"
#include "merizo/db/concurrency/lock_state.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/index_builds_coordinator_merizod.h"
#include "merizo/db/op_observer_registry.h"
#include "merizo/db/s/sharding_state.h"
#include "merizo/db/service_context.h"
#include "merizo/db/storage/storage_engine_init.h"
#include "merizo/dbtests/dbtests.h"
#include "merizo/dbtests/framework_options.h"
#include "merizo/scripting/dbdirectclient_factory.h"
#include "merizo/scripting/engine.h"
#include "merizo/stdx/mutex.h"
#include "merizo/util/assert_util.h"
#include "merizo/util/exit.h"
#include "merizo/util/log.h"
#include "merizo/util/periodic_runner_factory.h"
#include "merizo/util/scopeguard.h"
#include "merizo/util/version.h"

namespace merizo {
namespace dbtests {

int runDbTests(int argc, char** argv) {
    frameworkGlobalParams.perfHist = 1;
    frameworkGlobalParams.seed = time(0);
    frameworkGlobalParams.runsPerTest = 1;

    registerShutdownTask([] {
        // We drop the scope cache because leak sanitizer can't see across the
        // thread we use for proxying MozJS requests. Dropping the cache cleans up
        // the memory and makes leak sanitizer happy.
        ScriptEngine::dropScopeCache();

        // Shut down the background periodic task runner, before the storage engine.
        if (auto runner = getGlobalServiceContext()->getPeriodicRunner()) {
            runner->shutdown();
        }

        // We may be shut down before we have a global storage
        // engine.
        if (!getGlobalServiceContext()->getStorageEngine())
            return;

        shutdownGlobalStorageEngineCleanly(getGlobalServiceContext());
    });

    Client::initThread("testsuite");

    auto globalServiceContext = getGlobalServiceContext();

    // DBTests run as if in the database, so allow them to create direct clients.
    DBDirectClientFactory::get(globalServiceContext)
        .registerImplementation([](OperationContext* opCtx) {
            return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
        });

    srand((unsigned)frameworkGlobalParams.seed);

    // Set up the periodic runner for background job execution, which is required by the storage
    // engine to be running beforehand.
    auto runner = makePeriodicRunner(globalServiceContext);
    runner->startup();
    globalServiceContext->setPeriodicRunner(std::move(runner));

    initializeStorageEngine(globalServiceContext, StorageEngineInitFlags::kNone);
    DatabaseHolder::set(globalServiceContext, std::make_unique<DatabaseHolderImpl>());
    IndexBuildsCoordinator::set(globalServiceContext,
                                std::make_unique<IndexBuildsCoordinatorMongod>());
    auto registry = stdx::make_unique<OpObserverRegistry>();
    registry->addObserver(stdx::make_unique<UUIDCatalogObserver>());
    globalServiceContext->setOpObserver(std::move(registry));

    int ret = unittest::Suite::run(frameworkGlobalParams.suites,
                                   frameworkGlobalParams.filter,
                                   frameworkGlobalParams.runsPerTest);

    // So everything shuts down cleanly
    exitCleanly((ExitCode)ret);
    return ret;
}

}  // namespace dbtests

}  // namespace merizo
