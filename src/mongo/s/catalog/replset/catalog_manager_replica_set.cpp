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
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_actionlog.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
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

// Until read committed is supported always write to the primary with majority write and read
// from the secondary. That way we ensure that reads will see a consistent data.
const ReadPreferenceSetting kConfigWriteSelector(ReadPreference::PrimaryOnly, TagSet{});
const ReadPreferenceSetting kConfigReadSelector(ReadPreference::SecondaryPreferred, TagSet{});

const int kNotMasterNumRetries = 3;
const int kInitialSSVRetries = 3;
const Milliseconds kNotMasterRetryInterval{500};
const int kActionLogCollectionSize = 1024 * 1024 * 2;
const int kChangeLogCollectionSize = 1024 * 1024 * 10;

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

Status CatalogManagerReplicaSet::startup() {
    return Status::OK();
}

ConnectionString CatalogManagerReplicaSet::connectionString() const {
    return _configServerConnectionString;
}

void CatalogManagerReplicaSet::shutDown() {
    LOG(1) << "CatalogManagerReplicaSet::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    invariant(_distLockManager);
    _distLockManager->shutDown();
}

Status CatalogManagerReplicaSet::shardCollection(OperationContext* txn,
                                                 const string& ns,
                                                 const ShardKeyPattern& fieldsAndOrder,
                                                 bool unique,
                                                 const vector<BSONObj>& initPoints,
                                                 const set<ShardId>& initShardIds) {
    // Lock the collection globally so that no other mongos can try to shard or drop the collection
    // at the same time.
    auto scopedDistLock = getDistLockManager()->lock(ns, "shardCollection");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    StatusWith<DatabaseType> status = getDatabase(nsToDatabase(ns));
    if (!status.isOK()) {
        return status.getStatus();
    }

    DatabaseType dbt = status.getValue();
    ShardId dbPrimaryShardId = dbt.getPrimary();
    const auto primaryShard = grid.shardRegistry()->getShard(dbPrimaryShardId);

    {
        // In 3.0 and prior we include this extra safety check that the collection is not getting
        // sharded concurrently by two different mongos instances. It is not 100%-proof, but it
        // reduces the chance that two invocations of shard collection will step on each other's
        // toes.  Now we take the distributed lock so going forward this check won't be necessary
        // but we leave it around for compatibility with other mongoses from 3.0.
        // TODO(spencer): Remove this after 3.2 ships.
        const auto configShard = grid.shardRegistry()->getShard("config");
        const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
        if (!readHost.isOK()) {
            return readHost.getStatus();
        }

        auto countStatus = _runCountCommand(
            readHost.getValue(), NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)));
        if (!countStatus.isOK()) {
            return countStatus.getStatus();
        }
        if (countStatus.getValue() > 0) {
            return Status(ErrorCodes::AlreadyInitialized,
                          str::stream() << "collection " << ns << " already sharded with "
                                        << countStatus.getValue() << " chunks.");
        }
    }

    // Record start in changelog
    {
        BSONObjBuilder collectionDetail;
        collectionDetail.append("shardKey", fieldsAndOrder.toBSON());
        collectionDetail.append("collection", ns);
        collectionDetail.append("primary", primaryShard->toString());

        {
            BSONArrayBuilder initialShards(collectionDetail.subarrayStart("initShards"));
            for (const ShardId& shardId : initShardIds) {
                initialShards.append(shardId);
            }
        }

        collectionDetail.append("numChunks", static_cast<int>(initPoints.size() + 1));

        logChange(txn->getClient()->clientAddress(true),
                  "shardCollection.start",
                  ns,
                  collectionDetail.obj());
    }

    ChunkManagerPtr manager(new ChunkManager(ns, fieldsAndOrder, unique));
    manager->createFirstChunks(dbPrimaryShardId, &initPoints, &initShardIds);
    manager->loadExistingRanges(nullptr);

    CollectionInfo collInfo;
    collInfo.useChunkManager(manager);
    collInfo.save(ns);
    manager->reload(true);

    // TODO(spencer) SERVER-19319: Send setShardVersion to primary shard so it knows to start
    // rejecting unversioned writes.

    BSONObj finishDetail = BSON("version"
                                << "");  // TODO(spencer) SERVER-19319 Report actual version used

    logChange(txn->getClient()->clientAddress(true), "shardCollection", ns, finishDetail);

    return Status::OK();
}

StatusWith<ShardDrainingStatus> CatalogManagerReplicaSet::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
    const auto configShard = grid.shardRegistry()->getShard("config");
    const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    // Check preconditions for removing the shard
    auto countStatus =
        _runCountCommand(readHost.getValue(),
                         NamespaceString(ShardType::ConfigNS),
                         BSON(ShardType::name() << NE << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() > 0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      "Can't have more than one draining shard at a time");
    }

    countStatus = _runCountCommand(readHost.getValue(),
                                   NamespaceString(ShardType::ConfigNS),
                                   BSON(ShardType::name() << NE << name));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        return Status(ErrorCodes::IllegalOperation, "Can't remove last shard");
    }

    // Figure out if shard is already draining
    countStatus = _runCountCommand(readHost.getValue(),
                                   NamespaceString(ShardType::ConfigNS),
                                   BSON(ShardType::name() << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        log() << "going to start draining shard: " << name;

        Status status = update(ShardType::ConfigNS,
                               BSON(ShardType::name() << name),
                               BSON("$set" << BSON(ShardType::draining(true))),
                               false,  // upsert
                               false,  // multi
                               NULL);
        if (!status.isOK()) {
            log() << "error starting removeShard: " << name << "; err: " << status.reason();
            return status;
        }

        grid.shardRegistry()->reload();

        // Record start in changelog
        logChange(
            txn->getClient()->clientAddress(true), "removeShard.start", "", BSON("shard" << name));
        return ShardDrainingStatus::STARTED;
    }

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    countStatus = _runCountCommand(
        readHost.getValue(), NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long chunkCount = countStatus.getValue();

    countStatus = _runCountCommand(readHost.getValue(),
                                   NamespaceString(DatabaseType::ConfigNS),
                                   BSON(DatabaseType::primary(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long databaseCount = countStatus.getValue();

    if (chunkCount > 0 || databaseCount > 0) {
        // Still more draining to do
        return ShardDrainingStatus::ONGOING;
    }

    // Draining is done, now finish removing the shard.
    log() << "going to remove shard: " << name;
    audit::logRemoveShard(txn->getClient(), name);

    Status status = remove(ShardType::ConfigNS, BSON(ShardType::name() << name), 0, NULL);
    if (!status.isOK()) {
        log() << "Error concluding removeShard operation on: " << name
              << "; err: " << status.reason();
        return status;
    }

    grid.shardRegistry()->remove(name);
    grid.shardRegistry()->reload();

    // Record finish in changelog
    logChange(txn->getClient()->clientAddress(true), "removeShard", "", BSON("shard" << name));

    return ShardDrainingStatus::COMPLETED;
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
                                                           BSONObj(),
                                                           1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue();
    if (docs.empty()) {
        return {ErrorCodes::DatabaseNotFound, stream() << "database " << dbName << " not found"};
    }

    invariant(docs.size() == 1);

    return DatabaseType::fromBSON(docs.front());
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
                                             BSONObj(),
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
                                             BSONObj(),
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

void CatalogManagerReplicaSet::logChange(const string& clientAddress,
                                         const string& what,
                                         const string& ns,
                                         const BSONObj& detail) {
    if (_changeLogCollectionCreated.load() == 0) {
        BSONObj createCmd = BSON("create" << ChangeLogType::ConfigNS << "capped" << true << "size"
                                          << kChangeLogCollectionSize);
        auto result = _runConfigServerCommandWithNotMasterRetries("config", createCmd);
        if (!result.isOK()) {
            LOG(1) << "couldn't create changelog collection: " << causedBy(result.getStatus());
            return;
        }

        Status commandStatus = Command::getStatusFromCommandResult(result.getValue());
        if (commandStatus.isOK() || commandStatus == ErrorCodes::NamespaceExists) {
            _changeLogCollectionCreated.store(1);
        } else {
            LOG(1) << "couldn't create changelog collection: " << causedBy(commandStatus);
            return;
        }
    }

    Date_t now = grid.shardRegistry()->getExecutor()->now();
    std::string hostName = grid.shardRegistry()->getNetwork()->getHostName();
    const string changeId = str::stream() << hostName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(hostName);
    changeLog.setClientAddr(clientAddress);
    changeLog.setTime(now);
    changeLog.setNS(ns);
    changeLog.setWhat(what);
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    log() << "about to log metadata event: " << changeLogBSON;

    Status result = insert(ChangeLogType::ConfigNS, changeLogBSON, NULL);
    if (!result.isOK()) {
        warning() << "Error encountered while logging config change with ID " << changeId << ": "
                  << result;
    }
}

StatusWith<SettingsType> CatalogManagerReplicaSet::getGlobalSettings(const string& key) {
    const auto configShard = grid.shardRegistry()->getShard("config");
    const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                                           NamespaceString(SettingsType::ConfigNS),
                                                           BSON(SettingsType::key(key)),
                                                           BSONObj(),
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
                                                           BSONObj(),
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

Status CatalogManagerReplicaSet::getChunks(const BSONObj& query,
                                           const BSONObj& sort,
                                           boost::optional<int> limit,
                                           vector<ChunkType>* chunks) {
    chunks->clear();

    auto configShard = grid.shardRegistry()->getShard("config");
    auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHostStatus.isOK()) {
        return readHostStatus.getStatus();
    }

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = grid.shardRegistry()->exhaustiveFind(
        readHostStatus.getValue(), NamespaceString(ChunkType::ConfigNS), query, sort, longLimit);
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

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHostStatus.getValue(),
                                                           NamespaceString(TagsType::ConfigNS),
                                                           BSON(TagsType::ns(collectionNs)),
                                                           BSON(TagsType::min() << 1),
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
    auto configShard = grid.shardRegistry()->getShard("config");
    auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHostStatus.isOK()) {
        return readHostStatus.getStatus();
    }

    BSONObj query =
        BSON(TagsType::ns(collectionNs) << TagsType::min() << BSON("$lte" << chunk.getMin())
                                        << TagsType::max() << BSON("$gte" << chunk.getMax()));
    auto findStatus = grid.shardRegistry()->exhaustiveFind(
        readHostStatus.getValue(), NamespaceString(TagsType::ConfigNS), query, BSONObj(), 1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue();
    if (docs.empty()) {
        return string{};
    }

    invariant(docs.size() == 1);
    BSONObj tagsDoc = docs.front();

    const auto tagsResult = TagsType::fromBSON(tagsDoc);
    if (!tagsResult.isOK()) {
        return {ErrorCodes::FailedToParse,
                stream() << "error while parsing " << TagsType::ConfigNS << " document: " << tagsDoc
                         << " : " << tagsResult.getStatus().toString()};
    }
    return tagsResult.getValue().getTag();
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
                                                           BSONObj(),     // no sort
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

bool CatalogManagerReplicaSet::runReadCommand(const std::string& dbname,
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
    BSONObj cmd = BSON("applyOps" << updateOps << "preCondition" << preCondition);
    auto response = _runConfigServerCommandWithNotMasterRetries("config", cmd);

    if (!response.isOK()) {
        return response.getStatus();
    }

    Status status = Command::getStatusFromCommandResult(response.getValue());
    if (!status.isOK()) {
        string errMsg(str::stream() << "Unable to save chunk ops. Command: " << cmd
                                    << ". Result: " << response.getValue());

        return Status(status.code(), errMsg);
    }
    return Status::OK();
}

DistLockManager* CatalogManagerReplicaSet::getDistLockManager() const {
    invariant(_distLockManager);
    return _distLockManager.get();
}

void CatalogManagerReplicaSet::writeConfigServerDirect(const BatchedCommandRequest& batchRequest,
                                                       BatchedCommandResponse* batchResponse) {
    std::string dbname = batchRequest.getNS().db().toString();
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

Status CatalogManagerReplicaSet::_checkDbDoesNotExist(const string& dbName,
                                                      DatabaseType* db) const {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    const auto configShard = grid.shardRegistry()->getShard("config");
    const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                                           NamespaceString(DatabaseType::ConfigNS),
                                                           queryBuilder.obj(),
                                                           BSONObj(),
                                                           1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue();
    if (docs.empty()) {
        return Status::OK();
    }

    BSONObj dbObj = docs.front();
    std::string actualDbName = dbObj[DatabaseType::name()].String();
    if (actualDbName == dbName) {
        if (db) {
            auto parseDBStatus = DatabaseType::fromBSON(dbObj);
            if (!parseDBStatus.isOK()) {
                return parseDBStatus.getStatus();
            }

            *db = parseDBStatus.getValue();
        }

        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "database " << dbName << " already exists");
    }

    return Status(ErrorCodes::DatabaseDifferCase,
                  str::stream() << "can't have 2 databases that just differ on case "
                                << " have: " << actualDbName << " want to add: " << dbName);
}

StatusWith<std::string> CatalogManagerReplicaSet::_generateNewShardName() const {
    const auto configShard = grid.shardRegistry()->getShard("config");
    const auto readHost = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHost.isOK()) {
        return readHost.getStatus();
    }

    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost.getValue(),
                                                           NamespaceString(ShardType::ConfigNS),
                                                           shardNameRegex.obj(),
                                                           BSON(ShardType::name() << -1),
                                                           1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue();

    int count = 0;
    if (!docs.empty()) {
        const auto shardStatus = ShardType::fromBSON(docs.front());
        if (!shardStatus.isOK()) {
            return shardStatus.getStatus();
        }

        std::istringstream is(shardStatus.getValue().getName().substr(5));
        is >> count;
        count++;
    }

    // TODO fix so that we can have more than 10000 automatically generated shard names
    if (count < 9999) {
        std::stringstream ss;
        ss << "shard" << std::setfill('0') << std::setw(4) << count;
        return ss.str();
    }

    return Status(ErrorCodes::OperationFailed, "unable to generate new shard name");
}

StatusWith<long long> CatalogManagerReplicaSet::_runCountCommand(const HostAndPort& target,
                                                                 const NamespaceString& ns,
                                                                 BSONObj query) {
    BSONObj countCmd = BSON("count" << ns.coll() << "query" << query);
    auto responseStatus = grid.shardRegistry()->runCommand(target, ns.db().toString(), countCmd);
    if (!responseStatus.isOK()) {
        return responseStatus.getStatus();
    }

    auto responseObj = responseStatus.getValue();
    Status status = Command::getStatusFromCommandResult(responseObj);
    if (!status.isOK()) {
        return status;
    }

    long long result;
    status = bsonExtractIntegerField(responseObj, "n", &result);
    if (!status.isOK()) {
        return status;
    }

    return result;
}

Status CatalogManagerReplicaSet::checkAndUpgrade(bool checkOnly) {
    auto versionStatus = _getConfigVersion();
    if (!versionStatus.isOK()) {
        return versionStatus.getStatus();
    }

    auto versionInfo = versionStatus.getValue();
    if (versionInfo.getMinCompatibleVersion() > CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "current version v" << CURRENT_CONFIG_VERSION
                              << " is older than the cluster min compatible v"
                              << versionInfo.getMinCompatibleVersion()};
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion) {
        VersionType newVersion;
        newVersion.setClusterId(OID::gen());

        // For v3.2, only v3.2 binaries can talk to RS Config servers.
        newVersion.setMinCompatibleVersion(CURRENT_CONFIG_VERSION);
        newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

        BSONObj versionObj(newVersion.toBSON());

        return update(VersionType::ConfigNS,
                      versionObj,
                      versionObj,
                      true /* upsert*/,
                      false /* multi */,
                      nullptr);
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_UnreportedVersion) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                "Assuming config data is old since the version document cannot be found in the"
                "config server and it contains databases aside 'local' and 'admin'. "
                "Please upgrade if this is the case. Otherwise, make sure that the config "
                "server is clean."};
    }

    if (versionInfo.getCurrentVersion() < CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "need to upgrade current cluster version to v"
                              << CURRENT_CONFIG_VERSION << "; currently at v"
                              << versionInfo.getCurrentVersion()};
    }

    return Status::OK();
}

StatusWith<VersionType> CatalogManagerReplicaSet::_getConfigVersion() {
    const auto configShard = grid.shardRegistry()->getShard("config");
    const auto readHostStatus = configShard->getTargeter()->findHost(kConfigReadSelector);
    if (!readHostStatus.isOK()) {
        return readHostStatus.getStatus();
    }

    auto readHost = readHostStatus.getValue();
    auto findStatus = grid.shardRegistry()->exhaustiveFind(readHost,
                                                           NamespaceString(VersionType::ConfigNS),
                                                           BSONObj(),
                                                           BSONObj(),
                                                           boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue();

    if (queryResults.size() > 1) {
        return {ErrorCodes::RemoteValidationError,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS};
    }

    if (queryResults.empty()) {
        auto cmdStatus =
            grid.shardRegistry()->runCommand(readHost, "admin", BSON("listDatabases" << 1));
        if (!cmdStatus.isOK()) {
            return cmdStatus.getStatus();
        }

        const BSONObj& cmdResult = cmdStatus.getValue();

        Status cmdResultStatus = getStatusFromCommandResult(cmdResult);
        if (!cmdResultStatus.isOK()) {
            return cmdResultStatus;
        }

        for (const auto& dbEntry : cmdResult["databases"].Obj()) {
            const string& dbName = dbEntry["name"].String();

            if (dbName != "local" && dbName != "admin") {
                VersionType versionInfo;
                versionInfo.setMinCompatibleVersion(UpgradeHistory_UnreportedVersion);
                versionInfo.setCurrentVersion(UpgradeHistory_UnreportedVersion);
                return versionInfo;
            }
        }

        VersionType versionInfo;
        versionInfo.setMinCompatibleVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setCurrentVersion(UpgradeHistory_EmptyVersion);
        return versionInfo;
    }

    BSONObj versionDoc = queryResults.front();
    auto versionTypeResult = VersionType::fromBSON(versionDoc);
    if (!versionTypeResult.isOK()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream() << "invalid config version document: " << versionDoc
                                    << versionTypeResult.getStatus().toString());
    }

    return versionTypeResult.getValue();
}

}  // namespace mongo
