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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_logging.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/grid.h"

namespace mongo {

namespace {

const std::string kActionLogCollectionName("actionlog");
const int kActionLogCollectionSizeMB = 20 * 1024 * 1024;

const std::string kChangeLogCollectionName("changelog");
const int kChangeLogCollectionSizeMB = 200 * 1024 * 1024;

// Global ShardingLogging instance
const auto shardingLogging = ServiceContext::declareDecoration<ShardingLogging>();

}  // namespace

ShardingLogging::ShardingLogging() = default;

ShardingLogging::~ShardingLogging() = default;

ShardingLogging* ShardingLogging::get(ServiceContext* serviceContext) {
    return &shardingLogging(serviceContext);
}

ShardingLogging* ShardingLogging::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

Status ShardingLogging::logAction(OperationContext* opCtx,
                                  const StringData what,
                                  const StringData ns,
                                  const BSONObj& detail) {
    if (_actionLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(opCtx,
                                                      kActionLogCollectionName,
                                                      kActionLogCollectionSizeMB,
                                                      ShardingCatalogClient::kMajorityWriteConcern);
        if (result.isOK()) {
            _actionLogCollectionCreated.store(1);
        } else {
            LOGV2(22078,
                  "Couldn't create config.actionlog collection: {error}",
                  "Couldn't create config.actionlog collection",
                  "error"_attr = result);
            return result;
        }
    }

    return _log(opCtx,
                kActionLogCollectionName,
                what,
                ns,
                detail,
                ShardingCatalogClient::kMajorityWriteConcern);
}

Status ShardingLogging::logChangeChecked(OperationContext* opCtx,
                                         const StringData what,
                                         const StringData ns,
                                         const BSONObj& detail,
                                         const WriteConcernOptions& writeConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              writeConcern.wMode == WriteConcernOptions::kMajority);
    if (_changeLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(
            opCtx, kChangeLogCollectionName, kChangeLogCollectionSizeMB, writeConcern);
        if (result.isOK()) {
            _changeLogCollectionCreated.store(1);
        } else {
            LOGV2(22079,
                  "Couldn't create config.changelog collection: {error}",
                  "Couldn't create config.changelog collection",
                  "error"_attr = result);
            return result;
        }
    }

    return _log(opCtx, kChangeLogCollectionName, what, ns, detail, writeConcern);
}

Status ShardingLogging::_log(OperationContext* opCtx,
                             const StringData logCollName,
                             const StringData what,
                             const StringData operationNS,
                             const BSONObj& detail,
                             const WriteConcernOptions& writeConcern) {
    Date_t now = Grid::get(opCtx)->getNetwork()->now();
    const std::string serverName = str::stream()
        << Grid::get(opCtx)->getNetwork()->getHostName() << ":" << serverGlobalParams.port;
    const std::string changeId = str::stream()
        << serverName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(serverName);
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        changeLog.setShard("config");
    } else {
        auto shardingState = ShardingState::get(opCtx);
        if (shardingState->enabled()) {
            changeLog.setShard(shardingState->shardId().toString());
        }
    }
    changeLog.setClientAddr(opCtx->getClient()->clientAddress(true));
    changeLog.setTime(now);
    changeLog.setNS(operationNS.toString());
    changeLog.setWhat(what.toString());
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    LOGV2(22080,
          "About to log metadata event into {namespace}: {event}",
          "About to log metadata event",
          "namespace"_attr = logCollName,
          "event"_attr = redact(changeLogBSON));

    const NamespaceString nss("config", logCollName);
    Status result = Grid::get(opCtx)->catalogClient()->insertConfigDocument(
        opCtx, nss, changeLogBSON, writeConcern);

    if (!result.isOK()) {
        LOGV2_WARNING(22081,
                      "Error encountered while logging config change with ID [{changeId}] into "
                      "collection {namespace}: {error}",
                      "Error encountered while logging config change",
                      "changeId"_attr = changeId,
                      "namespace"_attr = logCollName,
                      "error"_attr = redact(result));
    }

    return result;
}

Status ShardingLogging::_createCappedConfigCollection(OperationContext* opCtx,
                                                      StringData collName,
                                                      int cappedSize,
                                                      const WriteConcernOptions& writeConcern) {
    BSONObj createCmd =
        BSON("create" << collName << "capped" << true << "size" << cappedSize
                      << WriteConcernOptions::kWriteConcernField << writeConcern.toBSON());

    auto result =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "config",
            createCmd,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent);

    if (!result.isOK()) {
        return result.getStatus();
    }

    if (!result.getValue().commandStatus.isOK()) {
        if (result.getValue().commandStatus == ErrorCodes::NamespaceExists) {
            if (result.getValue().writeConcernStatus.isOK()) {
                return Status::OK();
            } else {
                return result.getValue().writeConcernStatus;
            }
        } else {
            return result.getValue().commandStatus;
        }
    }

    return result.getValue().writeConcernStatus;
}

}  // namespace mongo
