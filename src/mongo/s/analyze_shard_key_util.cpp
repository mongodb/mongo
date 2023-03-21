/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/analyze_shard_key_util.h"

#include "mongo/client/connpool.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(analyzeShardKeyUtilHangBeforeExecutingCommandLocally);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyUtilHangBeforeExecutingCommandRemotely);

const int kMaxRetriesOnRetryableErrors = 5;

// The write concern for writes done as part of query sampling or analyzing a shard key.
const Seconds writeConcernTimeout{60};
const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, writeConcernTimeout};

/*
 * Returns true if this mongod can accept writes to the database 'dbName'. Unless it is the "local"
 * database, this will only return true if this mongod is a primary (or a standalone).
 */
bool canAcceptWrites(OperationContext* opCtx, const DatabaseName& dbName) {
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
    return mongo::repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
        opCtx, dbName.toString());
}

/*
 * Runs the command 'cmdObj' against the database 'dbName' locally. Then asserts that command
 * status using the 'uassertCmdStatusFn' callback. Returns the command response.
 */
BSONObj executeCommandOnPrimaryLocal(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    DBDirectClient client(opCtx);
    BSONObj resObj;
    client.runCommand(dbName, cmdObj, resObj);
    uassertCmdStatusFn(resObj);
    return resObj;
}

/*
 * Runs the command 'cmdObj' against the database 'dbName' on the (remote) primary. Then asserts
 * that the command status using the given 'uassertCmdStatusFn' callback. Throws a
 * PrimarySteppedDown error if no primary is found. Returns the command response.
 */
BSONObj executeCommandOnPrimaryRemote(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getCurrentPrimaryHostAndPort();

    if (hostAndPort.empty()) {
        uasserted(ErrorCodes::PrimarySteppedDown, "No primary exists currently");
    }

    auto conn = std::make_unique<ScopedDbConnection>(hostAndPort.toString());

    if (auth::isInternalAuthSet()) {
        uassertStatusOK(conn->get()->authenticateInternalUser());
    }

    DBClientBase* client = conn->get();
    ScopeGuard guard([&] { conn->done(); });
    try {
        BSONObj resObj;
        client->runCommand(dbName, cmdObj, resObj);
        uassertCmdStatusFn(resObj);
        return resObj;
    } catch (...) {
        guard.dismiss();
        conn->kill();
        throw;
    }
}

/*
 * The helper for 'validateCollectionOptions'. Performs the validation locally.
 */
StatusWith<UUID> validateCollectionOptionsLocal(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    if (CollectionCatalog::get(opCtx)->lookupView(opCtx, nss)) {
        return Status{ErrorCodes::CommandNotSupportedOnView, "The namespace corresponds to a view"};
    }

    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, nss);
    if (!collection) {
        return Status{ErrorCodes::NamespaceNotFound,
                      str::stream() << "The namespace does not exist"};
    }
    if (collection->getCollectionOptions().encryptedFieldConfig.has_value()) {
        return Status{ErrorCodes::IllegalOperation,
                      str::stream() << "The collection has queryable encryption enabled"};
    }
    return collection->uuid();
}

/*
 * The helper for 'validateCollectionOptions'. Performs the validation based on the listCollections
 * response from the primary shard for the database.
 */
StatusWith<UUID> validateCollectionOptionsOnPrimaryShard(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    ListCollections listCollections;
    listCollections.setDbName(nss.db());
    listCollections.setFilter(BSON("name" << nss.coll()));
    auto listCollectionsCmdObj =
        CommandHelpers::filterCommandRequestForPassthrough(listCollections.toBSON({}));

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    return shardVersionRetry(
        opCtx,
        catalogCache,
        nss,
        "validateCollectionOptionsOnPrimaryShard"_sd,
        [&]() -> StatusWith<UUID> {
            auto dbInfo = uassertStatusOK(catalogCache->getDatabaseWithRefresh(opCtx, nss.db()));
            auto cmdResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                nss.db(),
                dbInfo,
                listCollectionsCmdObj,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                Shard::RetryPolicy::kIdempotent);
            auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

            auto cursorResponse =
                uassertStatusOK(CursorResponse::parseFromBSON(remoteResponse.data));
            auto firstBatch = cursorResponse.getBatch();

            if (firstBatch.empty()) {
                return Status{ErrorCodes::NamespaceNotFound,
                              str::stream() << "The namespace does not exist"};
            }
            uassert(6915300,
                    str::stream() << "The namespace corresponds to multiple collections",
                    firstBatch.size() == 1);

            auto listCollRepItem = ListCollectionsReplyItem::parse(
                IDLParserContext("ListCollectionsReplyItem"), firstBatch[0]);

            if (listCollRepItem.getType() == "view") {
                return Status{ErrorCodes::CommandNotSupportedOnView,
                              "The namespace corresponds to a view"};
            }
            if (auto obj = listCollRepItem.getOptions()) {
                auto options = uassertStatusOK(CollectionOptions::parse(*obj));
                if (options.encryptedFieldConfig.has_value()) {
                    return Status{ErrorCodes::IllegalOperation,
                                  str::stream()
                                      << "The collection has queryable encryption enabled"};
                }
            }

            auto info = listCollRepItem.getInfo();
            uassert(6915301,
                    str::stream() << "The listCollections reply for '" << nss
                                  << "' does not have the 'info' field",
                    info);
            return *info->getUuid();
        });
}

}  // namespace

Status validateNamespace(const NamespaceString& nss) {
    if (nss.isOnInternalDb()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against an internal collection");
    }
    if (nss.isSystem()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against a system collection");
    }
    if (nss.isFLE2StateCollection()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot run against an internal collection");
    }
    return Status::OK();
}

StatusWith<UUID> validateCollectionOptions(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           StringData cmdName) {
    if (cmdName == AnalyzeShardKey::kCommandParameterFieldName) {
        return validateCollectionOptionsLocal(opCtx, nss);
    }
    if (cmdName == ConfigureQueryAnalyzer::kCommandParameterFieldName) {
        if (serverGlobalParams.clusterRole == ClusterRole::None) {
            return validateCollectionOptionsLocal(opCtx, nss);
        }
        tassert(7362503,
                str::stream()
                    << "Found the configureQueryAnalyzer command running on a shardsvr mongod",
                !serverGlobalParams.clusterRole.isExclusivelyShardRole());
        return validateCollectionOptionsOnPrimaryShard(opCtx, nss);
    }
    MONGO_UNREACHABLE;
}

Status validateIndexKey(const BSONObj& indexKey) {
    return validateShardKeyPattern(indexKey);
}

void uassertShardKeyValueNotContainArrays(const BSONObj& value) {
    for (const auto& element : value) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "The shard key contains an array field '" << element.fieldName()
                              << "'",
                element.type() != BSONType::Array);
    }
}

BSONObj extractReadConcern(OperationContext* opCtx) {
    return repl::ReadConcernArgs::get(opCtx).toBSONInner().removeField(
        ReadWriteConcernProvenanceBase::kSourceFieldName);
}

double round(double val, int n) {
    const double multiplier = std::pow(10.0, n);
    return std::ceil(val * multiplier) / multiplier;
}

double calculatePercentage(double part, double whole) {
    invariant(part >= 0);
    invariant(whole > 0);
    invariant(part <= whole);
    return round(part / whole * 100, kMaxNumDecimalPlaces);
}

BSONObj executeCommandOnPrimary(OperationContext* opCtx,
                                const DatabaseName& dbName,
                                const BSONObj& cmdObj,
                                const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    auto numRetries = 0;

    while (true) {
        try {
            if (canAcceptWrites(opCtx, dbName)) {
                // There is a window here where this mongod may step down after check above. In this
                // case, a NotWritablePrimary error would be thrown. However, this is preferable to
                // running the command while holding locks.
                analyzeShardKeyUtilHangBeforeExecutingCommandLocally.pauseWhileSet(opCtx);
                return executeCommandOnPrimaryLocal(opCtx, dbName, cmdObj, uassertCmdStatusFn);
            }

            analyzeShardKeyUtilHangBeforeExecutingCommandRemotely.pauseWhileSet(opCtx);
            return executeCommandOnPrimaryRemote(opCtx, dbName, cmdObj, uassertCmdStatusFn);
        } catch (DBException& ex) {
            if (ErrorCodes::isRetriableError(ex) && numRetries < kMaxRetriesOnRetryableErrors) {
                numRetries++;
                continue;
            }
            throw;
        }
    }

    return {};
}

void insertDocuments(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const std::vector<BSONObj>& docs,
                     const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    write_ops::InsertCommandRequest insertCmd(nss);
    insertCmd.setDocuments(docs);
    insertCmd.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(false);
        return wcb;
    }());
    auto insertCmdObj = insertCmd.toBSON(
        {BSON(WriteConcernOptions::kWriteConcernField << kMajorityWriteConcern.toBSON())});

    executeCommandOnPrimary(opCtx, nss.db(), std::move(insertCmdObj), [&](const BSONObj& resObj) {
        uassertCmdStatusFn(resObj);
    });
}

void dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    auto dropCollectionCmdObj =
        BSON("drop" << nss.coll().toString() << WriteConcernOptions::kWriteConcernField
                    << kMajorityWriteConcern.toBSON());
    executeCommandOnPrimary(
        opCtx, nss.db(), std::move(dropCollectionCmdObj), [&](const BSONObj& resObj) {
            BatchedCommandResponse res;
            std::string errMsg;

            if (!res.parseBSON(resObj, &errMsg)) {
                uasserted(ErrorCodes::FailedToParse, errMsg);
            }

            auto status = res.toStatus();
            if (status == ErrorCodes::NamespaceNotFound) {
                return;
            }
            uassertStatusOK(status);
        });
}

}  // namespace analyze_shard_key
}  // namespace mongo
