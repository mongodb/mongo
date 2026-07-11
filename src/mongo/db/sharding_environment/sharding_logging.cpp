// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/sharding_logging.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/type_changelog.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <string>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

const int kActionLogCollectionSizeMB = 20 * 1024 * 1024;
const int kChangeLogCollectionSizeMB = 200 * 1024 * 1024;

// Global ShardingLogging instance
const auto shardingLogging = ServiceContext::declareDecoration<ShardingLogging>();

}  // namespace

ShardingLogging* ShardingLogging::get(ServiceContext* serviceContext) {
    return &shardingLogging(serviceContext);
}

ShardingLogging* ShardingLogging::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

Status ShardingLogging::logAction(OperationContext* opCtx,
                                  const std::string_view what,
                                  const NamespaceString& ns,
                                  const BSONObj& detail,
                                  std::shared_ptr<Shard> configShard,
                                  ShardingCatalogClient* catalogClient) {
    auto configShardToUse =
        configShard ? std::move(configShard) : Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto catalogClientToUse = catalogClient ? catalogClient : Grid::get(opCtx)->catalogClient();

    if (_actionLogCollectionCreated.load() == 0) {
        Status result =
            _createCappedConfigCollection(opCtx,
                                          NamespaceString::kConfigActionlogNamespace.coll(),
                                          kActionLogCollectionSizeMB,
                                          defaultMajorityWriteConcernDoNotUse(),
                                          std::move(configShardToUse));
        if (result.isOK()) {
            _actionLogCollectionCreated.store(1);
        } else {
            LOGV2(22078, "Couldn't create config.actionlog collection", "error"_attr = result);
            return result;
        }
    }

    return _log(opCtx,
                NamespaceString::kConfigActionlogNamespace.coll(),
                what,
                ns,
                detail,
                defaultMajorityWriteConcernDoNotUse(),
                catalogClientToUse);
}

Status ShardingLogging::logChangeChecked(OperationContext* opCtx,
                                         const std::string_view what,
                                         const NamespaceString& ns,
                                         const BSONObj& detail,
                                         const WriteConcernOptions& writeConcern,
                                         std::shared_ptr<Shard> configShard,
                                         ShardingCatalogClient* catalogClient) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // If we're using a non-majority write concern, we should have provided an overriden
        // configShard and catalogClient to perform local operations.
        invariant(writeConcern.isMajority() || (configShard && catalogClient));
    } else {
        invariant(writeConcern.isMajority());
    }

    auto configShardToUse =
        configShard ? std::move(configShard) : Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto catalogClientToUse = catalogClient ? catalogClient : Grid::get(opCtx)->catalogClient();

    if (_changeLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(opCtx,
                                                      ChangeLogType::ConfigNS.coll(),
                                                      kChangeLogCollectionSizeMB,
                                                      writeConcern,
                                                      std::move(configShardToUse));
        if (result.isOK()) {
            _changeLogCollectionCreated.store(1);
        } else {
            LOGV2(22079, "Couldn't create config.changelog collection", "error"_attr = result);
            return result;
        }
    }

    return _log(
        opCtx, ChangeLogType::ConfigNS.coll(), what, ns, detail, writeConcern, catalogClientToUse);
}

Status ShardingLogging::_log(OperationContext* opCtx,
                             const std::string_view logCollName,
                             const std::string_view what,
                             const NamespaceString& operationNS,
                             const BSONObj& detail,
                             const WriteConcernOptions& writeConcern,
                             ShardingCatalogClient* catalogClient) {
    Date_t now = Grid::get(opCtx)->getNetwork()->now();

    const auto& session = opCtx->getClient()->session();
    const std::string sessionStr = session ? fmt::format(":{}", session->toBSON().toString()) : "";
    const std::string serverName = str::stream()
        << Grid::get(opCtx)->getNetwork()->getHostName() << sessionStr;
    const std::string changeId = str::stream()
        << serverName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(serverName);
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        changeLog.setShard("config");
    }
    auto shardingState = ShardingState::get(opCtx);
    if (shardingState->enabled()) {
        changeLog.setShard(shardingState->shardId().toString());
    }
    changeLog.setClientAddr(opCtx->getClient()->clientAddress(true));
    changeLog.setTime(now);
    changeLog.setNS(operationNS);
    changeLog.setWhat(std::string{what});
    // TODO SERVER-99655, SERVER-99552: update once gSnapshotFCVInDDLCoordinators is enabled on the
    // lastLTS and the OFCV is snapshotted for DDLs that do not pass by coordinators.
    if (auto& vCtx = VersionContext::getDecoration(opCtx); vCtx.hasOperationFCV()) {
        changeLog.setVersionContext(vCtx);
    }
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    LOGV2(22080,
          "About to log metadata event",
          "namespace"_attr = logCollName,
          "event"_attr = redact(changeLogBSON));

    const NamespaceString nss(NamespaceString::makeGlobalConfigCollection(logCollName));
    Status result = catalogClient->insertConfigDocument(opCtx, nss, changeLogBSON, writeConcern);

    if (!result.isOK()) {
        LOGV2_WARNING(5538900,
                      "Error encountered while logging config change",
                      "changeDocument"_attr = changeLog,
                      "error"_attr = redact(result));
    }

    return result;
}

Status ShardingLogging::_createCappedConfigCollection(OperationContext* opCtx,
                                                      std::string_view collName,
                                                      int cappedSize,
                                                      const WriteConcernOptions& writeConcern,
                                                      std::shared_ptr<Shard> configShard) {
    BSONObj createCmd =
        BSON("create" << collName << "capped" << true << "size" << cappedSize
                      << WriteConcernOptions::kWriteConcernField << writeConcern.toBSON());

    while (true) {
        auto result = configShard->runCommand(opCtx,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              DatabaseName::kConfig,
                                              createCmd,
                                              Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                              Shard::RetryPolicy::kIdempotent);

        if (!result.isOK()) {
            return result.getStatus();
        }

        if (!result.getValue().commandStatus.isOK()) {
            if (result.getValue().commandStatus == ErrorCodes::NamespaceExists) {
                if (result.getValue().writeConcernStatus.isOK()) {
                    return Status::OK();
                } else {
                    // Retry command in case of config server step down
                    continue;
                }
            } else {
                return result.getValue().commandStatus;
            }
        }

        return result.getValue().writeConcernStatus;
    }
}

}  // namespace mongo
