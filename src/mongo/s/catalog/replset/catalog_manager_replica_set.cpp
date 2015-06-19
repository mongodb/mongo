/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"

#include <pcrecpp.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/query_fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_runner.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using executor::TaskExecutor;
    using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;

    using std::set;
    using std::string;
    using std::unique_ptr;
    using std::vector;
    using str::stream;

namespace {

    const Status notYetImplemented(ErrorCodes::InternalError, "Not yet implemented"); // todo remove

    // Until read committed is supported always write to the primary with majoirty write and read
    // from the secondary. That way we ensure that reads will see a consistent data.
    const ReadPreferenceSetting kConfigWriteSelector(ReadPreference::PrimaryOnly, TagSet{});
    const ReadPreferenceSetting kConfigReadSelector(ReadPreference::SecondaryOnly, TagSet{});

    const Seconds kConfigCommandTimeout{30};
    const int kNotMasterNumRetries = 3;
    const Milliseconds kNotMasterRetryInterval{500};

    void _toBatchError(const Status& status, BatchedCommandResponse* response) {
        response->clear();
        response->setErrCode(status.code());
        response->setErrMessage(status.reason());
        response->setOk(false);
    }

} // namespace

    CatalogManagerReplicaSet::CatalogManagerReplicaSet() = default;

    CatalogManagerReplicaSet::~CatalogManagerReplicaSet() = default;

    Status CatalogManagerReplicaSet::init(const ConnectionString& configCS,
                                          std::unique_ptr<DistLockManager> distLockManager) {

        invariant(configCS.type() == ConnectionString::SET);

        _configServerConnectionString = configCS;
        _distLockManager = std::move(distLockManager);

        return Status::OK();
    }

    Status CatalogManagerReplicaSet::startup(bool upgrade) {
        return Status::OK();
    }

    ConnectionString CatalogManagerReplicaSet::connectionString() const {
        return _configServerConnectionString;
    }

    void CatalogManagerReplicaSet::shutDown() {
        LOG(1) << "CatalogManagerReplicaSet::shutDown() called.";
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _inShutdown = true;
        }

        invariant(_distLockManager);
        _distLockManager->shutDown();
    }

    Status CatalogManagerReplicaSet::enableSharding(const std::string& dbName) {
        return notYetImplemented;
    }

    Status CatalogManagerReplicaSet::shardCollection(const string& ns,
                                                     const ShardKeyPattern& fieldsAndOrder,
                                                     bool unique,
                                                     vector<BSONObj>* initPoints,
                                                     set<ShardId>* initShardsIds) {
        return notYetImplemented;
    }

    Status CatalogManagerReplicaSet::createDatabase(const std::string& dbName) {
        return notYetImplemented;
    }

    StatusWith<string> CatalogManagerReplicaSet::addShard(
            const string& name,
            const ConnectionString& shardConnectionString,
            const long long maxSize) {
        return notYetImplemented;
    }

    StatusWith<ShardDrainingStatus> CatalogManagerReplicaSet::removeShard(OperationContext* txn,
                                                                          const std::string& name) {
        return notYetImplemented;
    }

    Status CatalogManagerReplicaSet::updateDatabase(const std::string& dbName,
                                                    const DatabaseType& db) {
        fassert(28684, db.validate());

        return notYetImplemented;
    }

    StatusWith<DatabaseType> CatalogManagerReplicaSet::getDatabase(const std::string& dbName) {
        invariant(nsIsDbOnly(dbName));

        // The two databases that are hosted on the config server are config and admin
        if (dbName == "config" || dbName == "admin") {
            DatabaseType dbt;
            dbt.setName(dbName);
            dbt.setSharded(false);
            dbt.setPrimary("config");

            return dbt;
        }

        const auto configShard = grid.shardRegistry()->getShard("config");
        const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
        if (!readHost.isOK()) {
            return readHost.getStatus();
        }

        auto findStatus = _find(readHost.getValue(),
                                NamespaceString(DatabaseType::ConfigNS),
                                BSON(DatabaseType::name(dbName)),
                                1);
        if (!findStatus.isOK()) {
            return findStatus.getStatus();
        }

        const auto& docs = findStatus.getValue();
        if (docs.empty()) {
            return {ErrorCodes::NamespaceNotFound,
                    stream() << "database " << dbName << " not found"};
        }

        invariant(docs.size() == 1);

        return DatabaseType::fromBSON(docs.front());
    }

    Status CatalogManagerReplicaSet::updateCollection(const std::string& collNs,
                                                      const CollectionType& coll) {
        fassert(28683, coll.validate());

        BatchedCommandResponse response;
        Status status = update(CollectionType::ConfigNS,
                               BSON(CollectionType::fullNs(collNs)),
                               coll.toBSON(),
                               true,    // upsert
                               false,   // multi
                               &response);
        if (!status.isOK()) {
            return Status(status.code(),
                          str::stream() << "collection metadata write failed: "
                                        << response.toBSON() << "; status: " << status.toString());
        }

        return Status::OK();
    }

    StatusWith<CollectionType> CatalogManagerReplicaSet::getCollection(const std::string& collNs) {
        auto configShard = grid.shardRegistry()->getShard("config");

        auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
        if (!readHostStatus.isOK()) {
            return readHostStatus.getStatus();
        }

        auto statusFind = _find(readHostStatus.getValue(),
                                NamespaceString(CollectionType::ConfigNS),
                                BSON(CollectionType::fullNs(collNs)),
                                1);
        if (!statusFind.isOK()) {
            return statusFind.getStatus();
        }

        const auto& retVal = statusFind.getValue();
        if (retVal.empty()) {
            return Status(ErrorCodes::NamespaceNotFound,
                          stream() << "collection " << collNs << " not found");
        }

        invariant(retVal.size() == 1);

        return CollectionType::fromBSON(retVal.front());
    }

    Status CatalogManagerReplicaSet::getCollections(const std::string* dbName,
                                                    std::vector<CollectionType>* collections) {
        return notYetImplemented;
    }

    Status CatalogManagerReplicaSet::dropCollection(const std::string& collectionNs) {
        return notYetImplemented;
    }

    void CatalogManagerReplicaSet::logAction(const ActionLogType& actionLog) {

    }

    void CatalogManagerReplicaSet::logChange(OperationContext* opCtx,
                                             const string& what,
                                             const string& ns,
                                             const BSONObj& detail) {
    }

    StatusWith<SettingsType> CatalogManagerReplicaSet::getGlobalSettings(const string& key) {
        const auto configShard = grid.shardRegistry()->getShard("config");
        const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
        if (!readHost.isOK()) {
            return readHost.getStatus();
        }

        auto findStatus = _find(readHost.getValue(),
                                NamespaceString(SettingsType::ConfigNS),
                                BSON(SettingsType::key(key)),
                                1);
        if (!findStatus.isOK()) {
            return findStatus.getStatus();
        }

        const auto& docs = findStatus.getValue();
        if (docs.empty()) {
            return {ErrorCodes::NoMatchingDocument,
                    str::stream() << "can't find settings document with key: " << key};
        }

        BSONObj settingsDoc = docs.front();
        StatusWith<SettingsType> settingsResult = SettingsType::fromBSON(settingsDoc);
        if (!settingsResult.isOK()) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "error while parsing settings document: " << settingsDoc
                                  << " : " << settingsResult.getStatus().toString()};
        }

        const SettingsType& settings = settingsResult.getValue();

        Status validationStatus = settings.validate();
        if (!validationStatus.isOK()) {
            return validationStatus;
        }

        return settingsResult;
    }

    Status CatalogManagerReplicaSet::getDatabasesForShard(const string& shardName,
                                                        vector<string>* dbs) {
        return notYetImplemented;
    }

    Status CatalogManagerReplicaSet::getChunks(const Query& query,
                                               int nToReturn,
                                               vector<ChunkType>* chunks) {

        auto configShard = grid.shardRegistry()->getShard("config");
        auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
        if (!readHostStatus.isOK()) {
            return readHostStatus.getStatus();
        }

        auto findStatus = _find(readHostStatus.getValue(),
                                NamespaceString(ChunkType::ConfigNS),
                                query.obj,
                                0); // no limit
        if (!findStatus.isOK()) {
            return findStatus.getStatus();
        }

        for (const BSONObj& obj : findStatus.getValue()) {
            auto chunkRes = ChunkType::fromBSON(obj);
            if (!chunkRes.isOK()) {
                chunks->clear();
                return {ErrorCodes::FailedToParse,
                        stream() << "Failed to parse chunk with id ("
                                 << obj[ChunkType::name()].toString() << "): "
                                 << chunkRes.getStatus().reason()};
            }

            chunks->push_back(chunkRes.getValue());
        }

        return Status::OK();
    }

    Status CatalogManagerReplicaSet::getTagsForCollection(const std::string& collectionNs,
                                std::vector<TagsType>* tags) {
        return notYetImplemented;
    }

    StatusWith<string> CatalogManagerReplicaSet::getTagForChunk(const std::string& collectionNs,
                                                                const ChunkType& chunk) {
        return notYetImplemented;
    }

    Status CatalogManagerReplicaSet::getAllShards(vector<ShardType>* shards) {
        const auto configShard = grid.shardRegistry()->getShard("config");
        const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
        if (!readHost.isOK()) {
            return readHost.getStatus();
        }

        auto findStatus = _find(readHost.getValue(),
                                NamespaceString(ShardType::ConfigNS),
                                BSONObj(), // no query filter
                                0); // no limit
        if (!findStatus.isOK()) {
            return findStatus.getStatus();
        }

        for (const BSONObj& doc : findStatus.getValue()) {
            auto shardRes = ShardType::fromBSON(doc);
            if (!shardRes.isOK()) {
                shards->clear();
                return {ErrorCodes::FailedToParse,
                        stream() << "Failed to parse shard with id ("
                                 << doc[ShardType::name()].toString() << "): "
                                 << shardRes.getStatus().reason()};
            }

            shards->push_back(shardRes.getValue());
        }

        return Status::OK();
    }

    bool CatalogManagerReplicaSet::isShardHost(const ConnectionString& connectionString) {
        return false;
    }

    bool CatalogManagerReplicaSet::runUserManagementWriteCommand(const std::string& commandName,
                                                                 const std::string& dbname,
                                                                 const BSONObj& cmdObj,
                                                                 BSONObjBuilder* result) {
        auto scopedDistLock = getDistLockManager()->lock("authorizationData",
                                                         commandName,
                                                         Seconds{5});
        if (!scopedDistLock.isOK()) {
            return Command::appendCommandStatus(*result, scopedDistLock.getStatus());
        }

        auto targeter = grid.shardRegistry()->getShard("config")->getTargeter();

        Status notMasterStatus{ErrorCodes::InternalError, "status not set"};
        for (int i = 0; i < kNotMasterNumRetries; ++i) {

            auto target = targeter->findHost(kConfigWriteSelector);
            if (!target.isOK()) {
                if (ErrorCodes::NotMaster == target.getStatus()) {
                    notMasterStatus = target.getStatus();
                    sleepmillis(kNotMasterRetryInterval.count());
                    continue;
                }
                return Command::appendCommandStatus(*result, target.getStatus());
            }

            auto response = _runCommand(target.getValue(), dbname, cmdObj);
            if (!response.isOK()) {
                return Command::appendCommandStatus(*result, response.getStatus());
            }

            Status commandStatus = Command::getStatusFromCommandResult(response.getValue());
            if (ErrorCodes::NotMaster == commandStatus) {
                notMasterStatus = commandStatus;
                sleepmillis(kNotMasterRetryInterval.count());
                continue;
            }

            result->appendElements(response.getValue());

            return commandStatus.isOK();
        }

        invariant(ErrorCodes::NotMaster == notMasterStatus);
        return Command::appendCommandStatus(*result, notMasterStatus);
    }

    bool CatalogManagerReplicaSet::runUserManagementReadCommand(const std::string& dbname,
                                                                const BSONObj& cmdObj,
                                                                BSONObjBuilder* result) {
        auto targeter = grid.shardRegistry()->getShard("config")->getTargeter();
        auto target = targeter->findHost(kConfigReadSelector);
        if (!target.isOK()) {
            return Command::appendCommandStatus(*result, target.getStatus());
        }

        auto resultStatus = _runCommand(target.getValue(), dbname, cmdObj);
        if (!resultStatus.isOK()) {
            return Command::appendCommandStatus(*result, resultStatus.getStatus());
        }

        result->appendElements(resultStatus.getValue());

        return Command::getStatusFromCommandResult(resultStatus.getValue()).isOK();
        return false;
    }

    Status CatalogManagerReplicaSet::applyChunkOpsDeprecated(const BSONArray& updateOps,
                                                             const BSONArray& preCondition) {
        return notYetImplemented;
    }

    DistLockManager* CatalogManagerReplicaSet::getDistLockManager() {
        invariant(_distLockManager);
        return _distLockManager.get();
    }

    void CatalogManagerReplicaSet::writeConfigServerDirect(
            const BatchedCommandRequest& batchRequest,
            BatchedCommandResponse* batchResponse) {
        std::string dbname = batchRequest.getNSS().db().toString();
        invariant (dbname == "config" || dbname == "admin");
        const BSONObj cmdObj = batchRequest.toBSON();
        auto targeter = grid.shardRegistry()->getShard("config")->getTargeter();

        Status notMasterStatus{ErrorCodes::InternalError, "status not set"};
        for (int i = 0; i < kNotMasterNumRetries; ++i) {

            auto target = targeter->findHost(kConfigWriteSelector);
            if (!target.isOK()) {
                if (ErrorCodes::NotMaster == target.getStatus()) {
                    notMasterStatus = target.getStatus();
                    sleepmillis(kNotMasterRetryInterval.count());
                    continue;
                }
                _toBatchError(target.getStatus(), batchResponse);
                return;
            }

            auto resultStatus = _runCommand(target.getValue(),
                                            batchRequest.getNSS().db().toString(),
                                            batchRequest.toBSON());
            if (!resultStatus.isOK()) {
                _toBatchError(resultStatus.getStatus(), batchResponse);
                return;
            }

            const BSONObj& commandResponse = resultStatus.getValue();

            Status commandStatus = getStatusFromCommandResult(commandResponse);
            if (commandStatus == ErrorCodes::NotMaster) {
                notMasterStatus = commandStatus;
                sleepmillis(kNotMasterRetryInterval.count());
                continue;
            }

            string errmsg;
            if (!batchResponse->parseBSON(commandResponse, &errmsg)) {
                _toBatchError(Status(ErrorCodes::FailedToParse,
                                     str::stream() << "Failed to parse config server response: " <<
                                             errmsg),
                              batchResponse);
                return;
            }
            return; // The normal case return point.
        }

        invariant(ErrorCodes::NotMaster == notMasterStatus);
        _toBatchError(notMasterStatus, batchResponse);
    }

    StatusWith<vector<BSONObj>> CatalogManagerReplicaSet::_find(const HostAndPort& host,
                                                                const NamespaceString& nss,
                                                                const BSONObj& query,
                                                                int limit) {

        // If for some reason the callback never gets invoked, we will return this status
        Status status = Status(ErrorCodes::InternalError, "Internal error running find command");
        vector<BSONObj> results;

        auto fetcherCallback = [&status, &results](const QueryFetcher::BatchDataStatus& dataStatus,
                                                   Fetcher::NextAction* nextAction) {

            // Throw out any accumulated results on error
            if (!dataStatus.isOK()) {
                status = dataStatus.getStatus();
                results.clear();
                return;
            }

            auto& data = dataStatus.getValue();
            for (const BSONObj& doc : data.documents) {
                results.push_back(std::move(doc.getOwned()));
            }

            status = Status::OK();
        };

        unique_ptr<LiteParsedQuery> findCmd(
            fassertStatusOK(28688, LiteParsedQuery::makeAsFindCmd(nss.toString(), query, limit)));

        QueryFetcher fetcher(grid.shardRegistry()->getExecutor(),
                             host,
                             nss,
                             findCmd->asFindCommand(),
                             fetcherCallback);

        Status scheduleStatus = fetcher.schedule();
        if (!scheduleStatus.isOK()) {
            return scheduleStatus;
        }

        fetcher.wait();

        if (!status.isOK()) {
            return status;
        }

        return results;
    }

    StatusWith<BSONObj> CatalogManagerReplicaSet::_runCommand(const HostAndPort& host,
                                                              const std::string& dbName,
                                                              const BSONObj& cmdObj) {

        TaskExecutor* exec = grid.shardRegistry()->getExecutor();

        StatusWith<RemoteCommandResponse> responseStatus =
            Status(ErrorCodes::InternalError, "Internal error running command");

        RemoteCommandRequest request(host, dbName, cmdObj, kConfigCommandTimeout);
        auto callStatus =
            exec->scheduleRemoteCommand(request,
                                        [&responseStatus](const RemoteCommandCallbackArgs& args) {

            responseStatus = args.response;
        });

        if (!callStatus.isOK()) {
            return callStatus.getStatus();
        }

        // Block until the command is carried out
        exec->wait(callStatus.getValue());

        if (!responseStatus.isOK()) {
            return responseStatus.getStatus();
        }

        return responseStatus.getValue().data;
    }

} // namespace mongo
