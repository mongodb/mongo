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

#include "mongo/s/catalog/sharding_catalog_client_impl.h"

#include <iomanip>
#include <pcrecpp.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FP_DECLARE(failApplyChunkOps);
MONGO_FP_DECLARE(setDropCollDistLockWait);

using repl::OpTime;
using std::set;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using str::stream;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const ReadPreferenceSetting kConfigPrimaryPreferredSelector(ReadPreference::PrimaryPreferred,
                                                            TagSet{});
const int kMaxReadRetry = 3;
const int kMaxWriteRetry = 3;

const std::string kActionLogCollectionName("actionlog");
const int kActionLogCollectionSizeMB = 2 * 1024 * 1024;

const std::string kChangeLogCollectionName("changelog");
const int kChangeLogCollectionSizeMB = 10 * 1024 * 1024;

const NamespaceString kSettingsNamespace("config", "settings");

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
}

}  // namespace

ShardingCatalogClientImpl::ShardingCatalogClientImpl(
    std::unique_ptr<DistLockManager> distLockManager)
    : _distLockManager(std::move(distLockManager)) {}

ShardingCatalogClientImpl::~ShardingCatalogClientImpl() = default;

void ShardingCatalogClientImpl::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_started) {
        return;
    }

    _started = true;
    _distLockManager->startUp();
}

void ShardingCatalogClientImpl::shutDown(OperationContext* opCtx) {
    LOG(1) << "ShardingCatalogClientImpl::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    invariant(_distLockManager);
    _distLockManager->shutDown(opCtx);
}

Status ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
    OperationContext* opCtx,
    const std::string& collNs,
    const CollectionType& coll,
    const bool upsert) {
    fassert(28634, coll.validate());

    auto status = _updateConfigDocument(opCtx,
                                        CollectionType::ConfigNS,
                                        BSON(CollectionType::fullNs(collNs)),
                                        coll.toBSON(),
                                        upsert,
                                        ShardingCatalogClient::kMajorityWriteConcern);
    if (!status.isOK()) {
        return {status.getStatus().code(),
                str::stream() << "Collection metadata write failed due to "
                              << status.getStatus().reason()};
    }

    return Status::OK();
}

Status ShardingCatalogClientImpl::updateDatabase(OperationContext* opCtx,
                                                 const std::string& dbName,
                                                 const DatabaseType& db) {
    fassert(28616, db.validate());

    auto status = updateConfigDocument(opCtx,
                                       DatabaseType::ConfigNS,
                                       BSON(DatabaseType::name(dbName)),
                                       db.toBSON(),
                                       true,
                                       ShardingCatalogClient::kMajorityWriteConcern);
    if (!status.isOK()) {
        return {status.getStatus().code(),
                str::stream() << "Database metadata write failed due to "
                              << status.getStatus().reason()};
    }

    return Status::OK();
}

Status ShardingCatalogClientImpl::createDatabase(OperationContext* opCtx,
                                                 const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    // The admin and config databases should never be explicitly created. They "just exist",
    // i.e. getDatabase will always return an entry for them.
    invariant(dbName != "admin");
    invariant(dbName != "config");

    // Lock the database globally to prevent conflicts with simultaneous database creation.
    auto scopedDistLock = getDistLockManager()->lock(
        opCtx, dbName, "createDatabase", DistLockManager::kDefaultLockTimeout);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // check for case sensitivity violations
    Status status = _checkDbDoesNotExist(opCtx, dbName, nullptr);
    if (!status.isOK()) {
        return status;
    }

    // Database does not exist, pick a shard and create a new entry
    auto newShardIdStatus = _selectShardForNewDatabase(opCtx, Grid::get(opCtx)->shardRegistry());
    if (!newShardIdStatus.isOK()) {
        return newShardIdStatus.getStatus();
    }

    const ShardId& newShardId = newShardIdStatus.getValue();

    log() << "Placing [" << dbName << "] on: " << newShardId;

    DatabaseType db;
    db.setName(dbName);
    db.setPrimary(newShardId);
    db.setSharded(false);

    status = insertConfigDocument(
        opCtx, DatabaseType::ConfigNS, db.toBSON(), ShardingCatalogClient::kMajorityWriteConcern);
    if (status.code() == ErrorCodes::DuplicateKey) {
        return Status(ErrorCodes::NamespaceExists, "database " + dbName + " already exists");
    }

    return status;
}

Status ShardingCatalogClientImpl::logAction(OperationContext* opCtx,
                                            const std::string& what,
                                            const std::string& ns,
                                            const BSONObj& detail) {
    if (_actionLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(opCtx,
                                                      kActionLogCollectionName,
                                                      kActionLogCollectionSizeMB,
                                                      ShardingCatalogClient::kMajorityWriteConcern);
        if (result.isOK()) {
            _actionLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.actionlog collection:" << causedBy(result);
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

Status ShardingCatalogClientImpl::logChange(OperationContext* opCtx,
                                            const std::string& what,
                                            const std::string& ns,
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
            log() << "couldn't create config.changelog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(opCtx, kChangeLogCollectionName, what, ns, detail, writeConcern);
}

// static
StatusWith<ShardId> ShardingCatalogClientImpl::_selectShardForNewDatabase(
    OperationContext* opCtx, ShardRegistry* shardRegistry) {
    vector<ShardId> allShardIds;

    shardRegistry->getAllShardIds(&allShardIds);
    if (allShardIds.empty()) {
        shardRegistry->reload(opCtx);
        shardRegistry->getAllShardIds(&allShardIds);

        if (allShardIds.empty()) {
            return Status(ErrorCodes::ShardNotFound, "No shards found");
        }
    }

    ShardId candidateShardId = allShardIds[0];

    auto candidateSizeStatus = shardutil::retrieveTotalShardSize(opCtx, candidateShardId);
    if (!candidateSizeStatus.isOK()) {
        return candidateSizeStatus.getStatus();
    }

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const ShardId shardId = allShardIds[i];

        const auto sizeStatus = shardutil::retrieveTotalShardSize(opCtx, shardId);
        if (!sizeStatus.isOK()) {
            return sizeStatus.getStatus();
        }

        if (sizeStatus.getValue() < candidateSizeStatus.getValue()) {
            candidateSizeStatus = sizeStatus;
            candidateShardId = shardId;
        }
    }

    return candidateShardId;
}

Status ShardingCatalogClientImpl::_log(OperationContext* opCtx,
                                       const StringData& logCollName,
                                       const std::string& what,
                                       const std::string& operationNS,
                                       const BSONObj& detail,
                                       const WriteConcernOptions& writeConcern) {
    Date_t now = Grid::get(opCtx)->getNetwork()->now();
    const std::string hostName = Grid::get(opCtx)->getNetwork()->getHostName();
    const string changeId = str::stream() << hostName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(hostName);
    changeLog.setClientAddr(opCtx->getClient()->clientAddress(true));
    changeLog.setTime(now);
    changeLog.setNS(operationNS);
    changeLog.setWhat(what);
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    log() << "about to log metadata event into " << logCollName << ": " << redact(changeLogBSON);

    const NamespaceString nss("config", logCollName);
    Status result = insertConfigDocument(opCtx, nss.ns(), changeLogBSON, writeConcern);

    if (!result.isOK()) {
        warning() << "Error encountered while logging config change with ID [" << changeId
                  << "] into collection " << logCollName << ": " << redact(result);
    }

    return result;
}

StatusWith<ShardDrainingStatus> ShardingCatalogClientImpl::removeShard(OperationContext* opCtx,
                                                                       const ShardId& shardId) {
    // Check preconditions for removing the shard
    string name = shardId.toString();
    auto countStatus = _runCountCommandOnConfig(
        opCtx,
        NamespaceString(ShardType::ConfigNS),
        BSON(ShardType::name() << NE << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() > 0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      "Can't have more than one draining shard at a time");
    }

    countStatus = _runCountCommandOnConfig(
        opCtx, NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << name));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        return Status(ErrorCodes::IllegalOperation, "Can't remove last shard");
    }

    // Figure out if shard is already draining
    countStatus =
        _runCountCommandOnConfig(opCtx,
                                 NamespaceString(ShardType::ConfigNS),
                                 BSON(ShardType::name() << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    if (countStatus.getValue() == 0) {
        log() << "going to start draining shard: " << name;

        auto updateStatus = updateConfigDocument(opCtx,
                                                 ShardType::ConfigNS,
                                                 BSON(ShardType::name() << name),
                                                 BSON("$set" << BSON(ShardType::draining(true))),
                                                 false,
                                                 ShardingCatalogClient::kMajorityWriteConcern);
        if (!updateStatus.isOK()) {
            log() << "error starting removeShard: " << name
                  << causedBy(redact(updateStatus.getStatus()));
            return updateStatus.getStatus();
        }

        shardRegistry->reload(opCtx);

        // Record start in changelog
        logChange(opCtx,
                  "removeShard.start",
                  "",
                  BSON("shard" << name),
                  ShardingCatalogClientImpl::kMajorityWriteConcern)
            .transitional_ignore();

        return ShardDrainingStatus::STARTED;
    }

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    countStatus = _runCountCommandOnConfig(
        opCtx, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long chunkCount = countStatus.getValue();

    countStatus = _runCountCommandOnConfig(
        opCtx, NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(name)));
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
    audit::logRemoveShard(opCtx->getClient(), name);

    Status status = removeConfigDocuments(opCtx,
                                          ShardType::ConfigNS,
                                          BSON(ShardType::name() << name),
                                          ShardingCatalogClient::kMajorityWriteConcern);
    if (!status.isOK()) {
        log() << "Error concluding removeShard operation on: " << name
              << "; err: " << status.reason();
        return status;
    }

    shardConnectionPool.removeHost(name);
    ReplicaSetMonitor::remove(name);

    shardRegistry->reload(opCtx);

    // Record finish in changelog
    logChange(opCtx,
              "removeShard",
              "",
              BSON("shard" << name),
              ShardingCatalogClientImpl::kMajorityWriteConcern)
        .transitional_ignore();

    return ShardDrainingStatus::COMPLETED;
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::getDatabase(
    OperationContext* opCtx, const std::string& dbName) {
    if (!NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow)) {
        return {ErrorCodes::InvalidNamespace, stream() << dbName << " is not a valid db name"};
    }

    // The two databases that are hosted on the config server are config and admin
    if (dbName == "config" || dbName == "admin") {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setSharded(false);
        dbt.setPrimary(ShardId("config"));

        return repl::OpTimeWith<DatabaseType>(dbt);
    }

    auto result = _fetchDatabaseMetadata(opCtx, dbName, kConfigReadSelector);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        result = _fetchDatabaseMetadata(
            opCtx, dbName, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
        if (!result.isOK() && (result != ErrorCodes::NamespaceNotFound)) {
            return {result.getStatus().code(),
                    str::stream() << "Could not confirm non-existence of database " << dbName
                                  << " due to "
                                  << result.getStatus().reason()};
        }
    }

    return result;
}

StatusWith<repl::OpTimeWith<DatabaseType>> ShardingCatalogClientImpl::_fetchDatabaseMetadata(
    OperationContext* opCtx, const std::string& dbName, const ReadPreferenceSetting& readPref) {
    dassert(dbName != "admin" && dbName != "config");

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              readPref,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              BSON(DatabaseType::name(dbName)),
                                              BSONObj(),
                                              boost::none);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsWithOpTime = findStatus.getValue();
    if (docsWithOpTime.value.empty()) {
        return {ErrorCodes::NamespaceNotFound, stream() << "database " << dbName << " not found"};
    }

    invariant(docsWithOpTime.value.size() == 1);

    auto parseStatus = DatabaseType::fromBSON(docsWithOpTime.value.front());
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    return repl::OpTimeWith<DatabaseType>(parseStatus.getValue(), docsWithOpTime.opTime);
}

StatusWith<repl::OpTimeWith<CollectionType>> ShardingCatalogClientImpl::getCollection(
    OperationContext* opCtx, const std::string& collNs) {
    auto statusFind = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString(CollectionType::ConfigNS),
                                              BSON(CollectionType::fullNs(collNs)),
                                              BSONObj(),
                                              1);
    if (!statusFind.isOK()) {
        return statusFind.getStatus();
    }

    const auto& retOpTimePair = statusFind.getValue();
    const auto& retVal = retOpTimePair.value;
    if (retVal.empty()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "collection " << collNs << " not found");
    }

    invariant(retVal.size() == 1);

    auto parseStatus = CollectionType::fromBSON(retVal.front());
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    auto collType = parseStatus.getValue();
    if (collType.getDropped()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "collection " << collNs << " was dropped");
    }

    return repl::OpTimeWith<CollectionType>(collType, retOpTimePair.opTime);
}

Status ShardingCatalogClientImpl::getCollections(OperationContext* opCtx,
                                                 const std::string* dbName,
                                                 std::vector<CollectionType>* collections,
                                                 OpTime* opTime) {
    BSONObjBuilder b;
    if (dbName) {
        invariant(!dbName->empty());
        b.appendRegex(CollectionType::fullNs(),
                      string(str::stream() << "^" << pcrecpp::RE::QuoteMeta(*dbName) << "\\."));
    }

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString(CollectionType::ConfigNS),
                                              b.obj(),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docsOpTimePair = findStatus.getValue();

    for (const BSONObj& obj : docsOpTimePair.value) {
        const auto collectionResult = CollectionType::fromBSON(obj);
        if (!collectionResult.isOK()) {
            collections->clear();
            return {ErrorCodes::FailedToParse,
                    str::stream() << "error while parsing " << CollectionType::ConfigNS
                                  << " document: "
                                  << obj
                                  << " : "
                                  << collectionResult.getStatus().toString()};
        }

        collections->push_back(collectionResult.getValue());
    }

    if (opTime) {
        *opTime = docsOpTimePair.opTime;
    }

    return Status::OK();
}

Status ShardingCatalogClientImpl::dropCollection(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
    logChange(opCtx,
              "dropCollection.start",
              ns.ns(),
              BSONObj(),
              ShardingCatalogClientImpl::kMajorityWriteConcern)
        .transitional_ignore();

    auto shardsStatus = getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
    if (!shardsStatus.isOK()) {
        return shardsStatus.getStatus();
    }
    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    LOG(1) << "dropCollection " << ns << " started";

    // Lock the collection globally so that split/migrate cannot run
    Seconds waitFor(DistLockManager::kDefaultLockTimeout);
    MONGO_FAIL_POINT_BLOCK(setDropCollDistLockWait, customWait) {
        const BSONObj& data = customWait.getData();
        waitFor = Seconds(data["waitForSecs"].numberInt());
    }

    auto scopedDistLock = getDistLockManager()->lock(opCtx, ns.ns(), "drop", waitFor);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    LOG(1) << "dropCollection " << ns << " locked";

    const auto dropCommandBSON = [opCtx, &ns] {
        BSONObjBuilder builder;
        builder.append("drop", ns.coll());

        if (!opCtx->getWriteConcern().usedDefault) {
            builder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
        }

        return builder.obj();
    }();

    std::map<std::string, BSONObj> errors;
    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        auto swShard = shardRegistry->getShard(opCtx, shardEntry.getName());
        if (!swShard.isOK()) {
            return swShard.getStatus();
        }

        const auto& shard = swShard.getValue();

        auto swDropResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            ns.db().toString(),
            dropCommandBSON,
            Shard::RetryPolicy::kIdempotent);

        if (!swDropResult.isOK()) {
            return {swDropResult.getStatus().code(),
                    str::stream() << swDropResult.getStatus().reason() << " at "
                                  << shardEntry.getName()};
        }

        auto& dropResult = swDropResult.getValue();

        auto dropStatus = std::move(dropResult.commandStatus);
        auto wcStatus = std::move(dropResult.writeConcernStatus);
        if (!dropStatus.isOK() || !wcStatus.isOK()) {
            if (dropStatus.code() == ErrorCodes::NamespaceNotFound && wcStatus.isOK()) {
                // Generally getting NamespaceNotFound is okay to ignore as it simply means that
                // the collection has already been dropped or doesn't exist on this shard.
                // If, however, we get NamespaceNotFound but also have a write concern error then we
                // can't confirm whether the fact that the namespace doesn't exist is actually
                // committed.  Thus we must still fail on NamespaceNotFound if there is also a write
                // concern error. This can happen if we call drop, it succeeds but with a write
                // concern error, then we retry the drop.
                continue;
            }

            errors.emplace(shardEntry.getHost(), std::move(dropResult.response));
        }
    }

    if (!errors.empty()) {
        StringBuilder sb;
        sb << "Dropping collection failed on the following hosts: ";

        for (auto it = errors.cbegin(); it != errors.cend(); ++it) {
            if (it != errors.cbegin()) {
                sb << ", ";
            }

            sb << it->first << ": " << it->second;
        }

        return {ErrorCodes::OperationFailed, sb.str()};
    }

    LOG(1) << "dropCollection " << ns << " shard data deleted";

    // Remove chunk data
    Status result = removeConfigDocuments(opCtx,
                                          ChunkType::ConfigNS,
                                          BSON(ChunkType::ns(ns.ns())),
                                          ShardingCatalogClient::kMajorityWriteConcern);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << ns << " chunk data deleted";

    // Mark the collection as dropped
    CollectionType coll;
    coll.setNs(ns);
    coll.setDropped(true);
    coll.setEpoch(ChunkVersion::DROPPED().epoch());
    coll.setUpdatedAt(Grid::get(opCtx)->getNetwork()->now());

    const bool upsert = false;
    result = updateShardingCatalogEntryForCollection(opCtx, ns.ns(), coll, upsert);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << ns << " collection marked as dropped";

    for (const auto& shardEntry : allShards) {
        auto swShard = shardRegistry->getShard(opCtx, shardEntry.getName());
        if (!swShard.isOK()) {
            return swShard.getStatus();
        }

        const auto& shard = swShard.getValue();

        SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
            shardRegistry->getConfigServerConnectionString(),
            shardEntry.getName(),
            fassertStatusOK(28781, ConnectionString::parse(shardEntry.getHost())),
            ns,
            ChunkVersion::DROPPED(),
            true);

        auto ssvResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            ssv.toBSON(),
            Shard::RetryPolicy::kIdempotent);

        if (!ssvResult.isOK()) {
            return ssvResult.getStatus();
        }

        auto ssvStatus = std::move(ssvResult.getValue().commandStatus);
        if (!ssvStatus.isOK()) {
            return ssvStatus;
        }

        auto unsetShardingStatus = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON("unsetSharding" << 1),
            Shard::RetryPolicy::kIdempotent);

        if (!unsetShardingStatus.isOK()) {
            return unsetShardingStatus.getStatus();
        }

        auto unsetShardingResult = std::move(unsetShardingStatus.getValue().commandStatus);
        if (!unsetShardingResult.isOK()) {
            return unsetShardingResult;
        }
    }

    LOG(1) << "dropCollection " << ns << " completed";

    logChange(opCtx,
              "dropCollection",
              ns.ns(),
              BSONObj(),
              ShardingCatalogClientImpl::kMajorityWriteConcern)
        .transitional_ignore();

    return Status::OK();
}

StatusWith<BSONObj> ShardingCatalogClientImpl::getGlobalSettings(OperationContext* opCtx,
                                                                 StringData key) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              kSettingsNamespace,
                                              BSON("_id" << key),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return {ErrorCodes::NoMatchingDocument,
                str::stream() << "can't find settings document with key: " << key};
    }

    invariant(docs.size() == 1);
    return docs.front();
}

StatusWith<VersionType> ShardingCatalogClientImpl::getConfigVersion(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    auto findStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        readConcern,
        NamespaceString(VersionType::ConfigNS),
        BSONObj(),
        BSONObj(),
        boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().docs;

    if (queryResults.size() > 1) {
        return {ErrorCodes::TooManyMatchingDocuments,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS};
    }

    if (queryResults.empty()) {
        VersionType versionInfo;
        versionInfo.setMinCompatibleVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setCurrentVersion(UpgradeHistory_EmptyVersion);
        versionInfo.setClusterId(OID{});
        return versionInfo;
    }

    BSONObj versionDoc = queryResults.front();
    auto versionTypeResult = VersionType::fromBSON(versionDoc);
    if (!versionTypeResult.isOK()) {
        return {versionTypeResult.getStatus().code(),
                str::stream() << "Unable to parse config.version document " << versionDoc
                              << " due to "
                              << versionTypeResult.getStatus().reason()};
    }

    auto validationStatus = versionTypeResult.getValue().validate();
    if (!validationStatus.isOK()) {
        return Status(validationStatus.code(),
                      str::stream() << "Unable to validate config.version document " << versionDoc
                                    << " due to "
                                    << validationStatus.reason());
    }

    return versionTypeResult.getValue();
}

Status ShardingCatalogClientImpl::getDatabasesForShard(OperationContext* opCtx,
                                                       const ShardId& shardId,
                                                       vector<string>* dbs) {
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              BSON(DatabaseType::primary(shardId.toString())),
                                              BSONObj(),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& obj : findStatus.getValue().value) {
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

Status ShardingCatalogClientImpl::getChunks(OperationContext* opCtx,
                                            const BSONObj& query,
                                            const BSONObj& sort,
                                            boost::optional<int> limit,
                                            vector<ChunkType>* chunks,
                                            OpTime* opTime,
                                            repl::ReadConcernLevel readConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              readConcern == repl::ReadConcernLevel::kMajorityReadConcern);
    chunks->clear();

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcern,
                                              NamespaceString(ChunkType::ConfigNS),
                                              query,
                                              sort,
                                              longLimit);
    if (!findStatus.isOK()) {
        return {findStatus.getStatus().code(),
                str::stream() << "Failed to load chunks due to "
                              << findStatus.getStatus().reason()};
    }

    const auto& chunkDocsOpTimePair = findStatus.getValue();
    for (const BSONObj& obj : chunkDocsOpTimePair.value) {
        auto chunkRes = ChunkType::fromConfigBSON(obj);
        if (!chunkRes.isOK()) {
            chunks->clear();
            return {chunkRes.getStatus().code(),
                    stream() << "Failed to parse chunk with id " << obj[ChunkType::name()]
                             << " due to "
                             << chunkRes.getStatus().reason()};
        }

        chunks->push_back(chunkRes.getValue());
    }

    if (opTime) {
        *opTime = chunkDocsOpTimePair.opTime;
    }

    return Status::OK();
}

Status ShardingCatalogClientImpl::getTagsForCollection(OperationContext* opCtx,
                                                       const std::string& collectionNs,
                                                       std::vector<TagsType>* tags) {
    tags->clear();

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString(TagsType::ConfigNS),
                                              BSON(TagsType::ns(collectionNs)),
                                              BSON(TagsType::min() << 1),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return {findStatus.getStatus().code(),
                str::stream() << "Failed to load tags due to " << findStatus.getStatus().reason()};
    }

    const auto& tagDocsOpTimePair = findStatus.getValue();
    for (const BSONObj& obj : tagDocsOpTimePair.value) {
        auto tagRes = TagsType::fromBSON(obj);
        if (!tagRes.isOK()) {
            tags->clear();
            return {tagRes.getStatus().code(),
                    str::stream() << "Failed to parse tag with id " << obj[TagsType::tag()]
                                  << " due to "
                                  << tagRes.getStatus().toString()};
        }

        tags->push_back(tagRes.getValue());
    }

    return Status::OK();
}

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> ShardingCatalogClientImpl::getAllShards(
    OperationContext* opCtx, repl::ReadConcernLevel readConcern) {
    std::vector<ShardType> shards;
    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              readConcern,
                                              NamespaceString(ShardType::ConfigNS),
                                              BSONObj(),     // no query filter
                                              BSONObj(),     // no sort
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    for (const BSONObj& doc : findStatus.getValue().value) {
        auto shardRes = ShardType::fromBSON(doc);
        if (!shardRes.isOK()) {
            return {shardRes.getStatus().code(),
                    stream() << "Failed to parse shard document " << doc << " due to "
                             << shardRes.getStatus().reason()};
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return {validateStatus.code(),
                    stream() << "Failed to validate shard document " << doc << " due to "
                             << validateStatus.reason()};
        }

        shards.push_back(shardRes.getValue());
    }

    return repl::OpTimeWith<std::vector<ShardType>>{std::move(shards),
                                                    findStatus.getValue().opTime};
}

bool ShardingCatalogClientImpl::runUserManagementWriteCommand(OperationContext* opCtx,
                                                              const std::string& commandName,
                                                              const std::string& dbname,
                                                              const BSONObj& cmdObj,
                                                              BSONObjBuilder* result) {
    BSONObj cmdToRun = cmdObj;
    {
        // Make sure that if the command has a write concern that it is w:1 or w:majority, and
        // convert w:1 or no write concern to w:majority before sending.
        WriteConcernOptions writeConcern;
        writeConcern.reset();

        BSONElement writeConcernElement = cmdObj[WriteConcernOptions::kWriteConcernField];
        bool initialCmdHadWriteConcern = !writeConcernElement.eoo();
        if (initialCmdHadWriteConcern) {
            Status status = writeConcern.parse(writeConcernElement.Obj());
            if (!status.isOK()) {
                return Command::appendCommandStatus(*result, status);
            }

            if (!(writeConcern.wNumNodes == 1 ||
                  writeConcern.wMode == WriteConcernOptions::kMajority)) {
                return Command::appendCommandStatus(
                    *result,
                    {ErrorCodes::InvalidOptions,
                     str::stream() << "Invalid replication write concern. User management write "
                                      "commands may only use w:1 or w:'majority', got: "
                                   << writeConcern.toBSON()});
            }
        }

        writeConcern.wMode = WriteConcernOptions::kMajority;
        writeConcern.wNumNodes = 0;

        BSONObjBuilder modifiedCmd;
        if (!initialCmdHadWriteConcern) {
            modifiedCmd.appendElements(cmdObj);
        } else {
            BSONObjIterator cmdObjIter(cmdObj);
            while (cmdObjIter.more()) {
                BSONElement e = cmdObjIter.next();
                if (WriteConcernOptions::kWriteConcernField == e.fieldName()) {
                    continue;
                }
                modifiedCmd.append(e);
            }
        }
        modifiedCmd.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
        cmdToRun = modifiedCmd.obj();
    }

    auto response =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            dbname,
            cmdToRun,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kNotIdempotent);

    if (!response.isOK()) {
        return Command::appendCommandStatus(*result, response.getStatus());
    }
    if (!response.getValue().commandStatus.isOK()) {
        return Command::appendCommandStatus(*result, response.getValue().commandStatus);
    }
    if (!response.getValue().writeConcernStatus.isOK()) {
        return Command::appendCommandStatus(*result, response.getValue().writeConcernStatus);
    }

    Command::filterCommandReplyForPassthrough(response.getValue().response, result);
    return true;
}

bool ShardingCatalogClientImpl::runReadCommandForTest(OperationContext* opCtx,
                                                      const std::string& dbname,
                                                      const BSONObj& cmdObj,
                                                      BSONObjBuilder* result) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.appendElements(cmdObj);
    _appendReadConcern(&cmdBuilder);

    auto resultStatus =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx, kConfigReadSelector, dbname, cmdBuilder.done(), Shard::RetryPolicy::kIdempotent);
    if (resultStatus.isOK()) {
        result->appendElements(resultStatus.getValue().response);
        return resultStatus.getValue().commandStatus.isOK();
    }

    return Command::appendCommandStatus(*result, resultStatus.getStatus());
}

bool ShardingCatalogClientImpl::runUserManagementReadCommand(OperationContext* opCtx,
                                                             const std::string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
    auto resultStatus =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kConfigPrimaryPreferredSelector,
            dbname,
            cmdObj,
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent);
    if (resultStatus.isOK()) {
        Command::filterCommandReplyForPassthrough(resultStatus.getValue().response, result);
        return resultStatus.getValue().commandStatus.isOK();
    }

    return Command::appendCommandStatus(*result, resultStatus.getStatus());
}

Status ShardingCatalogClientImpl::applyChunkOpsDeprecated(OperationContext* opCtx,
                                                          const BSONArray& updateOps,
                                                          const BSONArray& preCondition,
                                                          const std::string& nss,
                                                          const ChunkVersion& lastChunkVersion,
                                                          const WriteConcernOptions& writeConcern,
                                                          repl::ReadConcernLevel readConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
              (readConcern == repl::ReadConcernLevel::kMajorityReadConcern &&
               writeConcern.wMode == WriteConcernOptions::kMajority));
    BSONObj cmd = BSON("applyOps" << updateOps << "preCondition" << preCondition
                                  << WriteConcernOptions::kWriteConcernField
                                  << writeConcern.toBSON());

    auto response =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "config",
            cmd,
            Shard::RetryPolicy::kIdempotent);

    if (!response.isOK()) {
        return response.getStatus();
    }

    Status status = response.getValue().commandStatus.isOK()
        ? std::move(response.getValue().writeConcernStatus)
        : std::move(response.getValue().commandStatus);

    // TODO (Dianna) This fail point needs to be reexamined when CommitChunkMigration is in:
    // migrations will no longer be able to exercise it, so split or merge will need to do so.
    // SERVER-22659.
    if (MONGO_FAIL_POINT(failApplyChunkOps)) {
        status = Status(ErrorCodes::InternalError, "Failpoint 'failApplyChunkOps' generated error");
    }

    if (!status.isOK()) {
        string errMsg;

        // This could be a blip in the network connectivity. Check if the commit request made it.
        //
        // If all the updates were successfully written to the chunks collection, the last
        // document in the list of updates should be returned from a query to the chunks
        // collection. The last chunk can be identified by namespace and version number.

        warning() << "chunk operation commit failed and metadata will be revalidated"
                  << causedBy(redact(status));

        // Look for the chunk in this shard whose version got bumped. We assume that if that
        // mod made it to the config server, then applyOps was successful.
        std::vector<ChunkType> newestChunk;
        BSONObjBuilder query;
        lastChunkVersion.addToBSON(query, ChunkType::lastmod());
        query.append(ChunkType::ns(), nss);
        Status chunkStatus =
            getChunks(opCtx, query.obj(), BSONObj(), 1, &newestChunk, nullptr, readConcern);

        if (!chunkStatus.isOK()) {
            errMsg = str::stream() << "getChunks function failed, unable to validate chunk "
                                   << "operation metadata: " << chunkStatus.toString()
                                   << ". applyChunkOpsDeprecated failed to get confirmation "
                                   << "of commit. Unable to save chunk ops. Command: " << cmd
                                   << ". Result: " << response.getValue().response;
        } else if (!newestChunk.empty()) {
            invariant(newestChunk.size() == 1);
            return Status::OK();
        } else {
            errMsg = str::stream() << "chunk operation commit failed: version "
                                   << lastChunkVersion.toString()
                                   << " doesn't exist in namespace: " << nss
                                   << ". Unable to save chunk ops. Command: " << cmd
                                   << ". Result: " << response.getValue().response;
        }

        return Status(status.code(), errMsg);
    }

    return Status::OK();
}

DistLockManager* ShardingCatalogClientImpl::getDistLockManager() {
    invariant(_distLockManager);
    return _distLockManager.get();
}

void ShardingCatalogClientImpl::writeConfigServerDirect(OperationContext* opCtx,
                                                        const BatchedCommandRequest& batchRequest,
                                                        BatchedCommandResponse* batchResponse) {
    // We only support batch sizes of one for config writes
    if (batchRequest.sizeWriteOps() != 1) {
        toBatchError(Status(ErrorCodes::InvalidOptions,
                            str::stream() << "Writes to config servers must have batch size of 1, "
                                          << "found "
                                          << batchRequest.sizeWriteOps()),
                     batchResponse);
        return;
    }

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    *batchResponse = configShard->runBatchWriteCommand(opCtx,
                                                       Shard::kDefaultConfigCommandTimeout,
                                                       batchRequest,
                                                       Shard::RetryPolicy::kNotIdempotent);
}

Status ShardingCatalogClientImpl::insertConfigDocument(OperationContext* opCtx,
                                                       const std::string& ns,
                                                       const BSONObj& doc,
                                                       const WriteConcernOptions& writeConcern) {
    const NamespaceString nss(ns);
    invariant(nss.db() == "config" || nss.db() == "admin");

    const BSONElement idField = doc.getField("_id");
    invariant(!idField.eoo());

    auto insert(stdx::make_unique<BatchedInsertRequest>());
    insert->addToDocuments(doc);

    BatchedCommandRequest request(insert.release());
    request.setNS(nss);
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        auto response = configShard->runBatchWriteCommand(
            opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kNoRetry);

        Status status = response.toStatus();

        if (retry < kMaxWriteRetry &&
            configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent)) {
            // Pretend like the operation is idempotent because we're handling DuplicateKey errors
            // specially
            continue;
        }

        // If we get DuplicateKey error on the first attempt to insert, this definitively means that
        // we are trying to insert the same entry a second time, so error out. If it happens on a
        // retry attempt though, it is not clear whether we are actually inserting a duplicate key
        // or it is because we failed to wait for write concern on the first attempt. In order to
        // differentiate, fetch the entry and check.
        if (retry > 1 && status == ErrorCodes::DuplicateKey) {
            LOG(1) << "Insert retry failed because of duplicate key error, rechecking.";

            auto fetchDuplicate =
                _exhaustiveFindOnConfig(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        repl::ReadConcernLevel::kMajorityReadConcern,
                                        nss,
                                        idField.wrap(),
                                        BSONObj(),
                                        boost::none);
            if (!fetchDuplicate.isOK()) {
                return fetchDuplicate.getStatus();
            }

            auto existingDocs = fetchDuplicate.getValue().value;
            if (existingDocs.empty()) {
                return {ErrorCodes::DuplicateKey,
                        stream() << "DuplicateKey error was returned after a retry attempt, but no "
                                    "documents were found. This means a concurrent change occurred "
                                    "together with the retries. Original error was "
                                 << status.toString()};
            }

            invariant(existingDocs.size() == 1);

            BSONObj existing = std::move(existingDocs.front());
            if (existing.woCompare(doc) == 0) {
                // Documents match, so treat the operation as success
                return Status::OK();
            }
        }

        return status;
    }

    MONGO_UNREACHABLE;
}

StatusWith<bool> ShardingCatalogClientImpl::updateConfigDocument(
    OperationContext* opCtx,
    const string& ns,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    return ShardingCatalogClientImpl::_updateConfigDocument(
        opCtx, ns, query, update, upsert, writeConcern);
}

StatusWith<bool> ShardingCatalogClientImpl::_updateConfigDocument(
    OperationContext* opCtx,
    const string& ns,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    const WriteConcernOptions& writeConcern) {
    const NamespaceString nss(ns);
    invariant(nss.db() == "config");

    const BSONElement idField = query.getField("_id");
    invariant(!idField.eoo());

    unique_ptr<BatchedUpdateDocument> updateDoc(new BatchedUpdateDocument());
    updateDoc->setQuery(query);
    updateDoc->setUpdateExpr(update);
    updateDoc->setUpsert(upsert);
    updateDoc->setMulti(false);

    unique_ptr<BatchedUpdateRequest> updateRequest(new BatchedUpdateRequest());
    updateRequest->addToUpdates(updateDoc.release());

    BatchedCommandRequest request(updateRequest.release());
    request.setNS(nss);
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);

    Status status = response.toStatus();
    if (!status.isOK()) {
        return status;
    }

    const auto nSelected = response.getN();
    invariant(nSelected == 0 || nSelected == 1);
    return (nSelected == 1);
}

Status ShardingCatalogClientImpl::removeConfigDocuments(OperationContext* opCtx,
                                                        const string& ns,
                                                        const BSONObj& query,
                                                        const WriteConcernOptions& writeConcern) {
    const NamespaceString nss(ns);
    invariant(nss.db() == "config");

    auto deleteDoc(stdx::make_unique<BatchedDeleteDocument>());
    deleteDoc->setQuery(query);
    deleteDoc->setLimit(0);

    auto deleteRequest(stdx::make_unique<BatchedDeleteRequest>());
    deleteRequest->addToDeletes(deleteDoc.release());

    BatchedCommandRequest request(deleteRequest.release());
    request.setNS(nss);
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);
    return response.toStatus();
}

Status ShardingCatalogClientImpl::_checkDbDoesNotExist(OperationContext* opCtx,
                                                       const string& dbName,
                                                       DatabaseType* db) {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    auto findStatus = _exhaustiveFindOnConfig(opCtx,
                                              kConfigReadSelector,
                                              repl::ReadConcernLevel::kMajorityReadConcern,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              queryBuilder.obj(),
                                              BSONObj(),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
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
                                << " have: "
                                << actualDbName
                                << " want to add: "
                                << dbName);
}

Status ShardingCatalogClientImpl::_createCappedConfigCollection(
    OperationContext* opCtx,
    StringData collName,
    int cappedSize,
    const WriteConcernOptions& writeConcern) {
    BSONObj createCmd = BSON("create" << collName << "capped" << true << "size" << cappedSize
                                      << WriteConcernOptions::kWriteConcernField
                                      << writeConcern.toBSON());

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

StatusWith<long long> ShardingCatalogClientImpl::_runCountCommandOnConfig(OperationContext* opCtx,
                                                                          const NamespaceString& ns,
                                                                          BSONObj query) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", ns.coll());
    countBuilder.append("query", query);
    _appendReadConcern(&countBuilder);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto resultStatus =
        configShard->runCommandWithFixedRetryAttempts(opCtx,
                                                      kConfigReadSelector,
                                                      ns.db().toString(),
                                                      countBuilder.done(),
                                                      Shard::kDefaultConfigCommandTimeout,
                                                      Shard::RetryPolicy::kIdempotent);
    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }
    if (!resultStatus.getValue().commandStatus.isOK()) {
        return resultStatus.getValue().commandStatus;
    }

    auto responseObj = std::move(resultStatus.getValue().response);

    long long result;
    auto status = bsonExtractIntegerField(responseObj, "n", &result);
    if (!status.isOK()) {
        return status;
    }

    return result;
}

StatusWith<repl::OpTimeWith<vector<BSONObj>>> ShardingCatalogClientImpl::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    repl::ReadConcernLevel readConcern,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto response = Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        opCtx, readPref, readConcern, nss, query, sort, limit);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return repl::OpTimeWith<vector<BSONObj>>(std::move(response.getValue().docs),
                                             response.getValue().opTime);
}

void ShardingCatalogClientImpl::_appendReadConcern(BSONObjBuilder* builder) {
    repl::ReadConcernArgs readConcern(grid.configOpTime(),
                                      repl::ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(builder);
}

Status ShardingCatalogClientImpl::appendInfoForConfigServerDatabases(
    OperationContext* opCtx, const BSONObj& listDatabasesCmd, BSONArrayBuilder* builder) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto resultStatus =
        configShard->runCommandWithFixedRetryAttempts(opCtx,
                                                      kConfigPrimaryPreferredSelector,
                                                      "admin",
                                                      listDatabasesCmd,
                                                      Shard::RetryPolicy::kIdempotent);

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }
    if (!resultStatus.getValue().commandStatus.isOK()) {
        return resultStatus.getValue().commandStatus;
    }

    auto listDBResponse = std::move(resultStatus.getValue().response);
    BSONElement dbListArray;
    auto dbListStatus = bsonExtractTypedField(listDBResponse, "databases", Array, &dbListArray);
    if (!dbListStatus.isOK()) {
        return dbListStatus;
    }

    BSONObjIterator iter(dbListArray.Obj());

    while (iter.more()) {
        auto dbEntry = iter.next().Obj();
        string name;
        auto parseStatus = bsonExtractStringField(dbEntry, "name", &name);

        if (!parseStatus.isOK()) {
            return parseStatus;
        }

        if (name == "config" || name == "admin") {
            builder->append(dbEntry);
        }
    }

    return Status::OK();
}

StatusWith<std::vector<KeysCollectionDocument>> ShardingCatalogClientImpl::getNewKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    repl::ReadConcernLevel readConcernLevel) {
    auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto findStatus =
        config->exhaustiveFindOnConfig(opCtx,
                                       kConfigReadSelector,
                                       readConcernLevel,
                                       NamespaceString(KeysCollectionDocument::ConfigNS),
                                       queryBuilder.obj(),
                                       BSON("expiresAt" << 1),
                                       boost::none);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& keyDocs = findStatus.getValue().docs;
    std::vector<KeysCollectionDocument> keys;
    for (auto&& keyDoc : keyDocs) {
        auto parseStatus = KeysCollectionDocument::fromBSON(keyDoc);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        keys.push_back(std::move(parseStatus.getValue()));
    }

    return keys;
}

}  // namespace mongo
