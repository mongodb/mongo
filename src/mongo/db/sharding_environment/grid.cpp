// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/grid.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_retry_server_parameters_gen.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/testing_proctor.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
const auto grid = ServiceContext::declareDecoration<Grid>();
}  // namespace

Grid::Grid() {
    ObservableMutexRegistry::get().add("gridMutex", _mutex);
}

Grid::~Grid() = default;

Grid* Grid::get(ServiceContext* serviceContext) {
    return &grid(serviceContext);
}

Grid* Grid::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

void Grid::init(std::unique_ptr<ShardingCatalogClient> catalogClient,
                std::unique_ptr<CatalogCache> catalogCache,
                std::shared_ptr<ShardRegistry> shardRegistry,
                std::unique_ptr<ClusterCursorManager> cursorManager,
                std::unique_ptr<BalancerConfiguration> balancerConfig,
                std::unique_ptr<executor::TaskExecutorPool> executorPool,
                executor::NetworkInterface* network) {
    invariant(!_catalogClient);
    invariant(!_catalogCache);
    invariant(!_shardRegistry);
    invariant(!_cursorManager);
    invariant(!_balancerConfig);
    invariant(!_executorPool);
    invariant(!_network);

    _catalogClient = std::move(catalogClient);
    _catalogCache = std::move(catalogCache);
    _shardRegistry = std::move(shardRegistry);
    _cursorManager = std::move(cursorManager);
    _balancerConfig = std::move(balancerConfig);
    _executorPool = std::move(executorPool);
    _network = network;

    _shardRegistry->init();

    _isGridInitialized.store(true);
}

bool Grid::isInitialized() const {
    return _isGridInitialized.load();
}

bool Grid::isShardingInitialized() const {
    return _shardingInitialized.load();
}

void Grid::assertShardingIsInitialized() const {
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "Sharding is not enabled",
            isShardingInitialized());
}

void Grid::setShardingInitialized() {
    invariant(!_shardingInitialized.load());
    _shardingInitialized.store(true);
}

Grid::CustomConnectionPoolStatsFn Grid::getCustomConnectionPoolStatsFn() const {
    std::lock_guard lk(_mutex);
    return _customConnectionPoolStatsFn;
}

void Grid::setCustomConnectionPoolStatsFn(CustomConnectionPoolStatsFn statsFn) {
    std::lock_guard lk(_mutex);
    invariant(!_customConnectionPoolStatsFn || !statsFn);
    _customConnectionPoolStatsFn = std::move(statsFn);
}

void Grid::shutdown(OperationContext* opCtx,
                    BSONObjBuilder* shutdownTimeElapsedBuilder,
                    bool isMongos) {
    if (!this->isInitialized()) {
        // Note that the Grid may not be initialized if a shutdown is triggered during the server
        // startup.
        return;
    }

    const auto serviceContext = opCtx->getServiceContext();

    if (auto cursorManager = this->getCursorManager()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownCursorManager,
                                       shutdownTimeElapsedBuilder);
        cursorManager->shutdown(opCtx);
    }

    // TODO (SERVER-50612): Shut down the ExecutorPool always. The shutdown of the ExecutorPool is
    // needed to prevent memory leaks. However, it can cause race conditions with sharding
    // components that use ScopedTaskExecutor. Since memory leaks are more desired than crashing at
    // shutdown, we decided to skip its shutdown on production but keep it on tests to have more
    // manageable BF tracking (see SERVER-78971 for more info).
    if (isMongos || TestingProctor::instance().isEnabled()) {
        if (auto pool = this->getExecutorPool()) {
            LOGV2(7698300, "Shutting down the ExecutorPool");
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownExecutorPool,
                                           shutdownTimeElapsedBuilder);
            pool->shutdownAndJoin();
        }
    }

    if (auto shardRegistry = this->shardRegistry()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownShardRegistry,
                                       shutdownTimeElapsedBuilder);
        LOGV2(4784919, "Shutting down the shard registry");
        shardRegistry->shutdown();
    }

    if (this->isShardingInitialized()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownCatalogCache,
                                       shutdownTimeElapsedBuilder);
        LOGV2(7698301, "Shutting down the CatalogCache");
        this->catalogCache()->shutDownAndJoin();
    }
}

void Grid::clearForUnitTests() {
    _catalogCache.reset();
    _shardRegistry.reset();
    _cursorManager.reset();
    _balancerConfig.reset();
    _executorPool.reset();
    _network = nullptr;
    _isGridInitialized.store(false);
    _shardingInitialized.store(false);
}

void Grid::setCatalogCache_forTest(std::unique_ptr<CatalogCache> catalogCache) {
    _catalogCache = std::move(catalogCache);
}

void Grid::setInitialized_forTest() {
    _isGridInitialized.store(true);
}

}  // namespace mongo
