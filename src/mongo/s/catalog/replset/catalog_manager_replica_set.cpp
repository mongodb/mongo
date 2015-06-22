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
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_actionlog.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::set;
using std::string;
using std::unique_ptr;
using std::vector;
using str::stream;

namespace {

const Status notYetImplemented(ErrorCodes::InternalError, "Not yet implemented");  // todo remove

// Until read committed is supported always write to the primary with majoirty write and read
// from the secondary. That way we ensure that reads will see a consistent data.
const ReadPreferenceSetting kConfigWriteSelector(ReadPreference::PrimaryOnly, TagSet{});
const ReadPreferenceSetting kConfigReadSelector(ReadPreference::SecondaryOnly, TagSet{});

const int kNotMasterNumRetries = 3;
const Milliseconds kNotMasterRetryInterval{500};
const int kActionLogCollectionSize = 1024 * 1024 * 2;

void _toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
}

}  // namespace

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

StatusWith<string> CatalogManagerReplicaSet::addShard(const string& name,
                                                      const ConnectionString& shardConnectionString,
                                                      const long long maxSize) {
    return notYetImplemented;
}

StatusWith<ShardDrainingStatus> CatalogManagerReplicaSet::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
    return notYetImplemented;
}

Status CatalogManagerReplicaSet::updateDatabase(const std::string& dbName, const DatabaseType& db) {
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

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                                           NamespaceString(DatabaseType::ConfigNS),
                                                           BSON(DatabaseType::name(dbName)),
                                                           1);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue();
    if (docs.empty()) {
        return {ErrorCodes::NamespaceNotFound, stream() << "database " << dbName << " not found"};
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
                           true,   // upsert
                           false,  // multi
                           &response);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "collection metadata write failed: " << response.toBSON()
                                    << "; status: " << status.toString());
    }

    return Status::OK();
}

StatusWith<CollectionType> CatalogManagerReplicaSet::getCollection(const std::string& collNs) {
    auto configShard = grid.shardRegistry()->getShard("config");

    auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHostStatus.isOK()) {
        return readHostStatus.getStatus();
    }

    auto statusFind =
        grid.shardRegistry()->exhaustiveFind(readHostStatus.getValue(),
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
    BSONObjBuilder b;
    if (dbName) {
        invariant(!dbName->empty());
        b.appendRegex(CollectionType::fullNs(),
                      string(str::stream() << "^" << pcrecpp::RE::QuoteMeta(*dbName) << "\\."));
    }

    auto configShard = grid.shardRegistry()->getShard("config");
    auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    auto findStatus =
        grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                             NamespaceString(CollectionType::ConfigNS),
                                             b.obj(),
                                             boost::none);  // no limit

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& obj : findStatus.getValue()) {
        const auto collectionResult = CollectionType::fromBSON(obj);
        if (!collectionResult.isOK()) {
            collections->clear();
            return {ErrorCodes::FailedToParse,
                    str::stream() << "error while parsing " << CollectionType::ConfigNS
                                  << " document: " << obj << " : "
                                  << collectionResult.getStatus().toString()};
        }

        collections->push_back(collectionResult.getValue());
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::dropCollection(const std::string& collectionNs) {
    return notYetImplemented;
}

void CatalogManagerReplicaSet::logAction(const ActionLogType& actionLog) {
    if (_actionLogCollectionCreated.load() == 0) {
        BSONObj createCmd = BSON("create" << ActionLogType::ConfigNS << "capped" << true << "size"
                                          << kActionLogCollectionSize);
        auto result = _runConfigServerCommandWithNotMasterRetries("config", createCmd);
        if (!result.isOK()) {
            LOG(1) << "couldn't create actionlog collection: " << causedBy(result.getStatus());
            return;
        }

        Status commandStatus = Command::getStatusFromCommandResult(result.getValue());
        if (commandStatus.isOK() || commandStatus == ErrorCodes::NamespaceExists) {
            _actionLogCollectionCreated.store(1);
        } else {
            LOG(1) << "couldn't create actionlog collection: " << causedBy(commandStatus);
            return;
        }
    }

    Status result = insert(ActionLogType::ConfigNS, actionLog.toBSON(), NULL);
    if (!result.isOK()) {
        log() << "error encountered while logging action: " << result;
    }
}

void CatalogManagerReplicaSet::logChange(OperationContext* opCtx,
                                         const string& what,
                                         const string& ns,
                                         const BSONObj& detail) {}

StatusWith<SettingsType> CatalogManagerReplicaSet::getGlobalSettings(const string& key) {
    const auto configShard = grid.shardRegistry()->getShard("config");
    const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
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
                str::stream() << "error while parsing settings document: " << settingsDoc << " : "
                              << settingsResult.getStatus().toString()};
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
    auto configShard = grid.shardRegistry()->getShard("config");
    auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                                           NamespaceString(DatabaseType::ConfigNS),
                                                           BSON(DatabaseType::primary(shardName)),
                                                           boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& obj : findStatus.getValue()) {
        string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::name(), &dbName);
        if (!status.isOK()) {
            dbs->clear();
            return status;
        }

        dbs->push_back(dbName);
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::getChunks(const Query& query,
                                           int nToReturn,
                                           vector<ChunkType>* chunks) {
    chunks->clear();

    auto configShard = grid.shardRegistry()->getShard("config");
    auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHostStatus.isOK()) {
        return readHostStatus.getStatus();
    }

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHostStatus.getValue(),
                                                           NamespaceString(ChunkType::ConfigNS),
                                                           query.obj,
                                                           boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& obj : findStatus.getValue()) {
        auto chunkRes = ChunkType::fromBSON(obj);
        if (!chunkRes.isOK()) {
            chunks->clear();
            return {ErrorCodes::FailedToParse,
                    stream() << "Failed to parse chunk with id ("
                             << obj[ChunkType::name()].toString()
                             << "): " << chunkRes.getStatus().toString()};
        }

        chunks->push_back(chunkRes.getValue());
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::getTagsForCollection(const std::string& collectionNs,
                                                      std::vector<TagsType>* tags) {
    tags->clear();

    auto configShard = grid.shardRegistry()->getShard("config");
    auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHostStatus.isOK()) {
        return readHostStatus.getStatus();
    }

    const Query query = Query(BSON(TagsType::ns(collectionNs))).sort(TagsType::min());

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHostStatus.getValue(),
                                                           NamespaceString(TagsType::ConfigNS),
                                                           query.obj,
                                                           boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }
    for (const BSONObj& obj : findStatus.getValue()) {
        auto tagRes = TagsType::fromBSON(obj);
        if (!tagRes.isOK()) {
            tags->clear();
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Failed to parse tag: " << tagRes.getStatus().toString());
        }

        tags->push_back(tagRes.getValue());
    }

    return Status::OK();
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

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                                           NamespaceString(ShardType::ConfigNS),
                                                           BSONObj(),     // no query filter
                                                           boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue()) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            shards->clear();
            return {ErrorCodes::FailedToParse,
                    stream() << "Failed to parse shard with id ("
                             << doc[ShardType::name()].toString()
                             << "): " << shardRes.getStatus().toString()};
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
    auto scopedDistLock = getDistLockManager()->lock("authorizationData", commandName, Seconds{5});
    if (!scopedDistLock.isOK()) {
        return Command::appendCommandStatus(*result, scopedDistLock.getStatus());
    }

    auto response = _runConfigServerCommandWithNotMasterRetries(dbname, cmdObj);
    if (!response.isOK()) {
        return Command::appendCommandStatus(*result, response.getStatus());
    }
    result->appendElements(response.getValue());
    return Command::getStatusFromCommandResult(response.getValue()).isOK();
}

bool CatalogManagerReplicaSet::runUserManagementReadCommand(const std::string& dbname,
                                                            const BSONObj& cmdObj,
                                                            BSONObjBuilder* result) {
    auto targeter = grid.shardRegistry()->getShard("config")->getTargeter();
    auto target = targeter->findHost(kConfigReadSelector);
    if (!target.isOK()) {
        return Command::appendCommandStatus(*result, target.getStatus());
    }

    auto resultStatus = grid.shardRegistry()->runCommand(target.getValue(), dbname, cmdObj);
    if (!resultStatus.isOK()) {
        return Command::appendCommandStatus(*result, resultStatus.getStatus());
    }

    result->appendElements(resultStatus.getValue());

    return Command::getStatusFromCommandResult(resultStatus.getValue()).isOK();
}

Status CatalogManagerReplicaSet::applyChunkOpsDeprecated(const BSONArray& updateOps,
                                                         const BSONArray& preCondition) {
    return notYetImplemented;
}

DistLockManager* CatalogManagerReplicaSet::getDistLockManager() const {
    invariant(_distLockManager);
    return _distLockManager.get();
}

void CatalogManagerReplicaSet::writeConfigServerDirect(const BatchedCommandRequest& batchRequest,
                                                       BatchedCommandResponse* batchResponse) {
    std::string dbname = batchRequest.getNSS().db().toString();
    invariant(dbname == "config" || dbname == "admin");
    const BSONObj cmdObj = batchRequest.toBSON();

    auto response = _runConfigServerCommandWithNotMasterRetries(dbname, cmdObj);
    if (!response.isOK()) {
        _toBatchError(response.getStatus(), batchResponse);
        return;
    }

    string errmsg;
    if (!batchResponse->parseBSON(response.getValue(), &errmsg)) {
        _toBatchError(Status(ErrorCodes::FailedToParse,
                             str::stream() << "Failed to parse config server response: " << errmsg),
                      batchResponse);
    }
}

StatusWith<BSONObj> CatalogManagerReplicaSet::_runConfigServerCommandWithNotMasterRetries(
    const std::string& dbname, const BSONObj& cmdObj) {
    auto targeter = grid.shardRegistry()->getShard("config")->getTargeter();

    for (int i = 0; i < kNotMasterNumRetries; ++i) {
        auto target = targeter->findHost(kConfigWriteSelector);
        if (!target.isOK()) {
            if (ErrorCodes::NotMaster == target.getStatus()) {
                if (i == kNotMasterNumRetries - 1) {
                    // If we're out of retries don't bother sleeping, just return.
                    return target.getStatus();
                }
                sleepmillis(kNotMasterRetryInterval.count());
                continue;
            }
            return target.getStatus();
        }

        auto response = grid.shardRegistry()->runCommand(target.getValue(), dbname, cmdObj);
        if (!response.isOK()) {
            return response.getStatus();
        }

        Status commandStatus = Command::getStatusFromCommandResult(response.getValue());
        if (ErrorCodes::NotMaster == commandStatus) {
            if (i == kNotMasterNumRetries - 1) {
                // If we're out of retries don't bother sleeping, just return.
                return commandStatus;
            }
            sleepmillis(kNotMasterRetryInterval.count());
            continue;
        }

        return response.getValue();
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
