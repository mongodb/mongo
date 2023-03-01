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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(analyzeShardKeyHangBeforeWritingLocally);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyHangBeforeWritingRemotely);

const int kMaxRetriesOnRetryableErrors = 5;

// The write concern for writes done as part of query sampling or analyzing a shard key.
const Seconds writeConcernTimeout{60};
const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, writeConcernTimeout};

/*
 * Returns true if this mongod can accept writes to the collection 'nss'. Unless the collection is
 * in the "local" database, this will only return true if this mongod is a primary (or a
 * standalone).
 */
bool canAcceptWrites(OperationContext* opCtx, const NamespaceString& nss) {
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    Lock::DBLock lk(opCtx, nss.dbName(), MODE_IS);
    Lock::CollectionLock lock(opCtx, nss, MODE_IS);
    return mongo::repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                       nss.db());
}

/*
 * Runs the write command 'cmdObj' against the database 'dbName' locally, asserts that the
 * top-level command is OK, then asserts the write status using the 'uassertWriteStatusFn' callback.
 * Returns the command response.
 */
BSONObj executeWriteCommandLocal(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj,
                                 const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
    DBDirectClient client(opCtx);
    BSONObj resObj;

    if (!client.runCommand(dbName, cmdObj, resObj)) {
        uassertStatusOK(getStatusFromCommandResult(resObj));
    }
    uassertWriteStatusFn(resObj);

    return resObj;
}

/*
 * Runs the write command 'cmdObj' against the database 'dbName' on the (remote) primary, asserts
 * that the top-level command is OK, then asserts the write status using the given
 * 'uassertWriteStatusFn' callback. Throws a PrimarySteppedDown error if no primary is found.
 * Returns the command response.
 */
BSONObj executeWriteCommandRemote(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const BSONObj& cmdObj,
                                  const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
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

        if (!client->runCommand(dbName, cmdObj, resObj)) {
            uassertStatusOK(getStatusFromCommandResult(resObj));
        }
        uassertWriteStatusFn(resObj);

        return resObj;
    } catch (...) {
        guard.dismiss();
        conn->kill();
        throw;
    }
}

/*
 * Runs the write command 'cmdObj' against the collection 'nss'. If this mongod is currently the
 * primary, runs the write command locally. Otherwise, runs the command on the remote primary.
 * Internally asserts that the top-level command is OK, then asserts the write status using the
 * given 'uassertWriteStatusFn' callback. Internally retries the write command on retryable
 * errors (for kMaxRetriesOnRetryableErrors times) so the writes must be idempotent. Returns the
 * command response.
 */
BSONObj executeWriteCommand(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
    const auto dbName = nss.dbName();
    auto numRetries = 0;

    while (true) {
        try {
            if (canAcceptWrites(opCtx, nss)) {
                // There is a window here where this mongod may step down after check above. In this
                // case, a NotWritablePrimary error would be thrown. However, this is preferable to
                // running the command while holding locks.
                analyzeShardKeyHangBeforeWritingLocally.pauseWhileSet(opCtx);
                return executeWriteCommandLocal(opCtx, dbName, cmdObj, uassertWriteStatusFn);
            }

            analyzeShardKeyHangBeforeWritingRemotely.pauseWhileSet(opCtx);
            return executeWriteCommandRemote(opCtx, dbName, cmdObj, uassertWriteStatusFn);
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
    auto dbInfo = uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));

    ListCollections listCollections;
    listCollections.setDbName(nss.db());
    listCollections.setFilter(BSON("name" << nss.coll()));
    auto cmdResponse = executeCommandAgainstDatabasePrimary(
        opCtx,
        nss.db(),
        dbInfo,
        CommandHelpers::filterCommandRequestForPassthrough(listCollections.toBSON({})),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);
    auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
    uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));

    auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(remoteResponse.data));
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
        return Status{ErrorCodes::CommandNotSupportedOnView, "The namespace corresponds to a view"};
    }
    if (auto obj = listCollRepItem.getOptions()) {
        auto options = uassertStatusOK(CollectionOptions::parse(*obj));
        if (options.encryptedFieldConfig.has_value()) {
            return Status{ErrorCodes::IllegalOperation,
                          str::stream() << "The collection has queryable encryption enabled"};
        }
    }

    auto info = listCollRepItem.getInfo();
    uassert(6915301,
            str::stream() << "The listCollections reply for '" << nss
                          << "' does not have the 'info' field",
            info);
    return *info->getUuid();
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

void insertDocuments(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const std::vector<BSONObj>& docs,
                     const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
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

    executeWriteCommand(opCtx, nss, std::move(insertCmdObj), [&](const BSONObj& resObj) {
        uassertWriteStatusFn(resObj);
    });
}

void dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    auto dropCollectionCmdObj =
        BSON("drop" << nss.coll().toString() << WriteConcernOptions::kWriteConcernField
                    << kMajorityWriteConcern.toBSON());
    executeWriteCommand(opCtx, nss, std::move(dropCollectionCmdObj), [&](const BSONObj& resObj) {
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
