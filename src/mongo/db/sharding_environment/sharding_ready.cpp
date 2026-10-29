// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/sharding_ready.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const auto shardingReady = ServiceContext::declareDecoration<ShardingReady>();

}  // namespace

ShardingReady* ShardingReady::get(ServiceContext* serviceContext) {
    return &shardingReady(serviceContext);
}

ShardingReady* ShardingReady::get(OperationContext* opCtx) {
    return ShardingReady::get(opCtx->getServiceContext());
}

void ShardingReady::scheduleTransitionToConfigShard(OperationContext* opCtx) {
    auto catalogManager = ShardingCatalogManager::get(opCtx);
    auto shards =
        catalogManager->localCatalogClient()->getAllShards(opCtx, repl::ReadConcernArgs::kLocal);

    // Only transition to config shard if we have no existing data shards. Otherwise, we could end
    // up transitioning back to config shard after the user called transition to dedicated config
    // server.
    if (shards.value.empty()) {
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();

        // The ShardingReady::_isReady promise will be indirectly set after the config server
        // has transitioned into a config shard. This happens in the config_server_op_observer which
        // sets the _isReady promise when it sees that a shard with _id "config" has been added to
        // config.shards (which only occurs after transition to config shard has completed).
        (void)AsyncTry([this, serviceContext = opCtx->getServiceContext()] {
            _transitionToConfigShard(serviceContext);
        })
            .until([](Status status) {
                if (!status.isOK()) {
                    LOGV2_WARNING(7910801,
                                  "Failed to transition to config shard during "
                                  "autobootstrap due to error. Retrying.",
                                  "error"_attr = status);
                }
                // Keep retrying until the transition to config shard succeeds, the
                // node is shutting down, or is no longer primary.
                return status.isOK() || ErrorCodes::isShutdownError(status) ||
                    ErrorCodes::isNotPrimaryError(status);
            })
            .withDelayBetweenIterations(Milliseconds(500))
            .on(executor, CancellationToken::uncancelable());
    }
}

void ShardingReady::_transitionToConfigShard(ServiceContext* serviceContext) {
    // Since this function is async, we need to create a new client and operation context to run
    // 'transitionFromDedicatedConfigServer'.
    auto clientGuard =
        ClientStrand::make(serviceContext->getService()->makeClient("ShardingReady"))->bind();
    auto uniqueOpCtx = clientGuard->makeOperationContext();

    auto as = AuthorizationSession::get(uniqueOpCtx->getClient());
    as->grantInternalAuthorization();

    FixedFCVRegion fixedFcvRegion(uniqueOpCtx.get());
    ShardingCatalogManager::get(uniqueOpCtx.get())
        ->addConfigShard(uniqueOpCtx.get(), fixedFcvRegion);
    LOGV2(7910800, "Auto-bootstrap to config shard complete.");
}

void ShardingReady::waitUntilReady(OperationContext* opCtx) {
    _isReady.getFuture().get(opCtx);
}

bool ShardingReady::isReady() {
    return _isReady.getFuture().isReady();
}

SharedSemiFuture<void> ShardingReady::isReadyFuture() const {
    return _isReady.getFuture();
}

void ShardingReady::setIsReady() {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_isReady.getFuture().isReady()) {
        _isReady.emplaceValue();
    }
}

void ShardingReady::setIsReadyIfShardExists(OperationContext* opCtx) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

    auto configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();
    auto shardFindResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::Nearest},
                                            repl::ReadConcernArgs::kLocal,
                                            NamespaceString::kConfigsvrShardsNamespace,
                                            BSONObj(), /* Find all shards */
                                            BSONObj() /* No sorting */,
                                            boost::none /* No limit */));

    if (shardFindResponse.docs.size() > 0) {
        setIsReady();
    }
}

}  // namespace mongo
