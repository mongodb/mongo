// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/dbtests/framework.h"

#include "mongo/client/dbclient_base.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/collection_impl.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/database_holder_impl.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/dbtests/framework_options.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_main_core.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::dbtests {

unittest::MainProgress initializeDbTests(std::vector<std::string> argVec) {
    unittest::MainProgress progress({}, std::move(argVec));
    progress.initialize();

    if (auto suites = getFrameworkSuites(); !suites.empty()) {
        auto& o = progress.options().filter;
        if (!o)
            o.emplace();
        LOGV2(11723900, "Dbtest overriding suites.", "suites"_attr = suites);
        o->suites = std::move(suites);
    }

    return progress;
}

int runDbTests(unittest::MainProgress& progress) {
    auto serviceContext = getGlobalServiceContext();

    registerShutdownTask([serviceContext] {
        // We drop the scope cache because leak sanitizer can't see across the thread we use for
        // proxying MozJS requests. Dropping the cache cleans up the memory and makes leak sanitizer
        // happy.
        ScriptEngine::dropScopeCache();

        // We may be shut down before we have a global storage engine.
        if (!serviceContext->getStorageEngine())
            return;

        catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(serviceContext,
                                                                        true /* memLeakAllowed */);
    });

    ThreadClient tc("testsuite", serviceContext->getService());

    // DBTests run as if in the database, so allow them to create direct clients.
    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    srand(static_cast<unsigned>(time(nullptr)));  // NOLINT

    // Set up the periodic runner for background job execution, which is required by the storage
    // engine to be running beforehand.
    auto runner = makePeriodicRunner(serviceContext);
    serviceContext->setPeriodicRunner(std::move(runner));

    catalog::startUpStorageEngineAndCollectionCatalog(serviceContext, &cc());
    StorageControl::startStorageControls(serviceContext, true /*forTestOnly*/);
    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImplFactory>());
    IndexBuildsCoordinator::set(serviceContext, std::make_unique<IndexBuildsCoordinatorMongod>());
    auto registry = std::make_unique<OpObserverRegistry>();
    serviceContext->setOpObserver(std::move(registry));
    ShardingState::create(serviceContext);
    CollectionShardingStateFactory::set(
        serviceContext, std::make_unique<CollectionShardingStateFactoryShard>(serviceContext));
    DatabaseShardingStateFactory::set(serviceContext,
                                      std::make_unique<DatabaseShardingStateFactoryShard>());

    int ret = progress.test();

    // So everything shuts down cleanly
    CollectionShardingStateFactory::clear(serviceContext);
    DatabaseShardingStateFactory::clear(serviceContext);
    exitCleanly((ExitCode)ret);
    return ret;
}

}  // namespace mongo::dbtests
