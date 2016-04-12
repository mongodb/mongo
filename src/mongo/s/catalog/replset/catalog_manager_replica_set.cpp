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
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/dist_lock_manager.h"
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

const char kWriteConcernField[] = "writeConcern";

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const ReadPreferenceSetting kConfigPrimaryPreferredSelector(ReadPreference::PrimaryPreferred,
                                                            TagSet{});
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplicaSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

const int kMaxConfigVersionInitRetry = 3;
const int kMaxReadRetry = 3;
const int kMaxWriteRetry = 3;

const std::string kActionLogCollectionName("actionlog");
const int kActionLogCollectionSizeMB = 2 * 1024 * 1024;

const std::string kChangeLogCollectionName("changelog");
const int kChangeLogCollectionSizeMB = 10 * 1024 * 1024;

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
}

/**
 * Validates that the specified connection string can serve as a shard server. In particular,
 * this function checks that the shard can be contacted, that it is not already member of
 * another sharded cluster and etc.
 *
 * @param shardRegistry Shard registry to use for opening connections to the shards.
 * @param connectionString Connection string to be attempted as a shard host.
 * @param shardProposedName Optional proposed name for the shard. Can be omitted in which case
 *      a unique name for the shard will be generated from the shard's connection string. If it
 *      is not omitted, the value cannot be the empty string.
 *
 * On success returns a partially initialized shard type object corresponding to the requested
 * shard. It will have the hostName field set and optionally the name, if the name could be
 * generated from either the proposed name or the connection string set name. The returned
 * shard's name should be checked and if empty, one should be generated using some uniform
 * algorithm.
 */
StatusWith<ShardType> validateHostAsShard(OperationContext* txn,
                                          ShardRegistry* shardRegistry,
                                          const ConnectionString& connectionString,
                                          const std::string* shardProposedName) {
    if (connectionString.type() == ConnectionString::INVALID) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    const std::shared_ptr<Shard> shardConn{shardRegistry->createConnection(connectionString)};
    invariant(shardConn);

    const ReadPreferenceSetting readPref{ReadPreference::PrimaryOnly};

    // Is it mongos?
    auto cmdStatus = shardRegistry->runIdempotentCommandForAddShard(
        txn, shardConn, readPref, "admin", BSON("isdbgrid" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    // (ok == 1) implies that it is a mongos
    if (getStatusFromCommandResult(cmdStatus.getValue()).isOK()) {
        return {ErrorCodes::OperationFailed, "can't add a mongos process as a shard"};
    }

    // Is it a replica set?
    cmdStatus = shardRegistry->runIdempotentCommandForAddShard(
        txn, shardConn, readPref, "admin", BSON("isMaster" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    BSONObj resIsMaster = cmdStatus.getValue();

    const string providedSetName = connectionString.getSetName();
    const string foundSetName = resIsMaster["setName"].str();

    // Make sure the specified replica set name (if any) matches the actual shard's replica set
    if (providedSetName.empty() && !foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host is part of set " << foundSetName << "; "
                              << "use replica set url format "
                              << "<setname>/<server1>,<server2>, ..."};
    }

    if (!providedSetName.empty() && foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host did not return a set name; "
                              << "is the replica set still initializing? " << resIsMaster};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name " << foundSetName};
    }

    // Is it a mongos config server?
    cmdStatus = shardRegistry->runIdempotentCommandForAddShard(
        txn, shardConn, readPref, "admin", BSON("replSetGetStatus" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    BSONObj res = cmdStatus.getValue();

    if (getStatusFromCommandResult(res).isOK()) {
        bool isConfigServer;
        Status status =
            bsonExtractBooleanFieldWithDefault(res, "configsvr", false, &isConfigServer);
        if (!status.isOK()) {
            return Status(status.code(),
                          str::stream() << "replSetGetStatus returned invalid \"configsvr\" "
                                        << "field when attempting to add "
                                        << connectionString.toString()
                                        << " as a shard: " << status.reason());
        }

        if (isConfigServer) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Cannot add " << connectionString.toString()
                                  << " as a shard since it is part of a config server replica set"};
        }
    } else if ((res["info"].type() == String) && (res["info"].String() == "configsvr")) {
        return {ErrorCodes::OperationFailed,
                "the specified mongod is a legacy-style config "
                "server and cannot be used as a shard server"};
    }

    // If the shard is part of a replica set, make sure all the hosts mentioned in the connection
    // string are part of the set. It is fine if not all members of the set are mentioned in the
    // connection string, though.
    if (!providedSetName.empty()) {
        std::set<string> hostSet;

        BSONObjIterator iter(resIsMaster["hosts"].Obj());
        while (iter.more()) {
            hostSet.insert(iter.next().String());  // host:port
        }

        if (resIsMaster["passives"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["passives"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        if (resIsMaster["arbiters"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["arbiters"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        vector<HostAndPort> hosts = connectionString.getServers();
        for (size_t i = 0; i < hosts.size(); i++) {
            const string host = hosts[i].toString();  // host:port
            if (hostSet.find(host) == hostSet.end()) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "in seed list " << connectionString.toString() << ", host "
                                      << host << " does not belong to replica set " << foundSetName
                                      << "; found " << resIsMaster.toString()};
            }
        }
    }

    string actualShardName;

    if (shardProposedName) {
        actualShardName = *shardProposedName;
    } else if (!foundSetName.empty()) {
        // Default it to the name of the replica set
        actualShardName = foundSetName;
    }

    // Disallow adding shard replica set with name 'config'
    if (actualShardName == "config") {
        return {ErrorCodes::BadValue, "use of shard replica set with name 'config' is not allowed"};
    }

    // Retrieve the most up to date connection string that we know from the replica set monitor (if
    // this is a replica set shard, otherwise it will be the same value as connectionString).
    ConnectionString actualShardConnStr = shardConn->getTargeter()->connectionString();

    ShardType shard;
    shard.setName(actualShardName);
    shard.setHost(actualShardConnStr.toString());

    return shard;
}

/**
 * Runs the listDatabases command on the specified host and returns the names of all databases
 * it returns excluding those named local and admin, since they serve administrative purpose.
 */
StatusWith<std::vector<std::string>> getDBNamesListFromShard(
    OperationContext* txn, ShardRegistry* shardRegistry, const ConnectionString& connectionString) {
    const std::shared_ptr<Shard> shardConn{
        shardRegistry->createConnection(connectionString).release()};
    invariant(shardConn);

    auto cmdStatus = shardRegistry->runIdempotentCommandForAddShard(
        txn,
        shardConn,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        BSON("listDatabases" << 1));
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    const BSONObj& cmdResult = cmdStatus.getValue();

    Status cmdResultStatus = getStatusFromCommandResult(cmdResult);
    if (!cmdResultStatus.isOK()) {
        return cmdResultStatus;
    }

    vector<string> dbNames;

    for (const auto& dbEntry : cmdResult["databases"].Obj()) {
        const string& dbName = dbEntry["name"].String();

        if (!(dbName == "local" || dbName == "admin")) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

}  // namespace

CatalogManagerReplicaSet::CatalogManagerReplicaSet(std::unique_ptr<DistLockManager> distLockManager)
    : _distLockManager(std::move(distLockManager)) {}

CatalogManagerReplicaSet::~CatalogManagerReplicaSet() = default;

Status CatalogManagerReplicaSet::startup(OperationContext* txn) {
    _distLockManager->startUp();
    return Status::OK();
}

void CatalogManagerReplicaSet::shutDown(OperationContext* txn) {
    LOG(1) << "CatalogManagerReplicaSet::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    invariant(_distLockManager);
    _distLockManager->shutDown(txn);
}

StatusWith<string> CatalogManagerReplicaSet::addShard(OperationContext* txn,
                                                      const std::string* shardProposedName,
                                                      const ConnectionString& shardConnectionString,
                                                      const long long maxSize) {
    // Validate the specified connection string may serve as shard at all
    auto shardStatus =
        validateHostAsShard(txn, grid.shardRegistry(), shardConnectionString, shardProposedName);
    if (!shardStatus.isOK()) {
        // TODO: This is a workaround for the case were we could have some bad shard being
        // requested to be added and we put that bad connection string on the global replica set
        // monitor registry. It needs to be cleaned up so that when a correct replica set is added,
        // it will be recreated.
        ReplicaSetMonitor::remove(shardConnectionString.getSetName());
        return shardStatus.getStatus();
    }

    ShardType& shardType = shardStatus.getValue();

    auto dbNamesStatus = getDBNamesListFromShard(txn, grid.shardRegistry(), shardConnectionString);
    if (!dbNamesStatus.isOK()) {
        return dbNamesStatus.getStatus();
    }

    // Check that none of the existing shard candidate's dbs exist already
    for (const string& dbName : dbNamesStatus.getValue()) {
        auto dbt = getDatabase(txn, dbName);
        if (dbt.isOK()) {
            const auto& dbDoc = dbt.getValue().value;
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "can't add shard "
                                        << "'" << shardConnectionString.toString() << "'"
                                        << " because a local database '" << dbName
                                        << "' exists in another " << dbDoc.getPrimary());
        } else if (dbt != ErrorCodes::NamespaceNotFound) {
            return dbt.getStatus();
        }
    }

    // If a name for a shard wasn't provided, generate one
    if (shardType.getName().empty()) {
        StatusWith<string> result = _generateNewShardName(txn);
        if (!result.isOK()) {
            return Status(ErrorCodes::OperationFailed, "error generating new shard name");
        }

        shardType.setName(result.getValue());
    }

    if (maxSize > 0) {
        shardType.setMaxSizeMB(maxSize);
    }

    log() << "going to add shard: " << shardType.toString();

    Status result = insertConfigDocument(txn, ShardType::ConfigNS, shardType.toBSON());
    if (!result.isOK()) {
        log() << "error adding shard: " << shardType.toBSON() << " err: " << result.reason();
        return result;
    }

    // Make sure the new shard is visible
    grid.shardRegistry()->reload(txn);

    // Add all databases which were discovered on the new shard
    for (const string& dbName : dbNamesStatus.getValue()) {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setPrimary(shardType.getName());
        dbt.setSharded(false);

        Status status = updateDatabase(txn, dbName, dbt);
        if (!status.isOK()) {
            log() << "adding shard " << shardConnectionString.toString()
                  << " even though could not add database " << dbName;
        }
    }

    // Record in changelog
    BSONObjBuilder shardDetails;
    shardDetails.append("name", shardType.getName());
    shardDetails.append("host", shardConnectionString.toString());

    logChange(txn, "addShard", "", shardDetails.obj());

    return shardType.getName();
}

Status CatalogManagerReplicaSet::updateCollection(OperationContext* txn,
                                                  const std::string& collNs,
                                                  const CollectionType& coll) {
    fassert(28634, coll.validate());

    auto status = updateConfigDocument(
        txn, CollectionType::ConfigNS, BSON(CollectionType::fullNs(collNs)), coll.toBSON(), true);
    if (!status.isOK()) {
        return Status(status.getStatus().code(),
                      str::stream() << "collection metadata write failed"
                                    << causedBy(status.getStatus()));
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::updateDatabase(OperationContext* txn,
                                                const std::string& dbName,
                                                const DatabaseType& db) {
    fassert(28616, db.validate());

    auto status = updateConfigDocument(
        txn, DatabaseType::ConfigNS, BSON(DatabaseType::name(dbName)), db.toBSON(), true);
    if (!status.isOK()) {
        return Status(status.getStatus().code(),
                      str::stream() << "database metadata write failed"
                                    << causedBy(status.getStatus()));
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::createDatabase(OperationContext* txn, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    // The admin and config databases should never be explicitly created. They "just exist",
    // i.e. getDatabase will always return an entry for them.
    invariant(dbName != "admin");
    invariant(dbName != "config");

    // Lock the database globally to prevent conflicts with simultaneous database creation.
    auto scopedDistLock = getDistLockManager()->lock(txn, dbName, "createDatabase");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // check for case sensitivity violations
    Status status = _checkDbDoesNotExist(txn, dbName, nullptr);
    if (!status.isOK()) {
        return status;
    }

    // Database does not exist, pick a shard and create a new entry
    auto newShardIdStatus = _selectShardForNewDatabase(txn, grid.shardRegistry());
    if (!newShardIdStatus.isOK()) {
        return newShardIdStatus.getStatus();
    }

    const ShardId& newShardId = newShardIdStatus.getValue();

    log() << "Placing [" << dbName << "] on: " << newShardId;

    DatabaseType db;
    db.setName(dbName);
    db.setPrimary(newShardId);
    db.setSharded(false);

    status = insertConfigDocument(txn, DatabaseType::ConfigNS, db.toBSON());
    if (status.code() == ErrorCodes::DuplicateKey) {
        return Status(ErrorCodes::NamespaceExists, "database " + dbName + " already exists");
    }

    return status;
}

Status CatalogManagerReplicaSet::logAction(OperationContext* txn,
                                           const std::string& what,
                                           const std::string& ns,
                                           const BSONObj& detail) {
    if (_actionLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(
            txn, kActionLogCollectionName, kActionLogCollectionSizeMB);
        if (result.isOK() || result == ErrorCodes::NamespaceExists) {
            _actionLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.actionlog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(txn, kActionLogCollectionName, what, ns, detail);
}

Status CatalogManagerReplicaSet::logChange(OperationContext* txn,
                                           const std::string& what,
                                           const std::string& ns,
                                           const BSONObj& detail) {
    if (_changeLogCollectionCreated.load() == 0) {
        Status result = _createCappedConfigCollection(
            txn, kChangeLogCollectionName, kChangeLogCollectionSizeMB);
        if (result.isOK() || result == ErrorCodes::NamespaceExists) {
            _changeLogCollectionCreated.store(1);
        } else {
            log() << "couldn't create config.changelog collection:" << causedBy(result);
            return result;
        }
    }

    return _log(txn, kChangeLogCollectionName, what, ns, detail);
}

// static
StatusWith<ShardId> CatalogManagerReplicaSet::_selectShardForNewDatabase(
    OperationContext* txn, ShardRegistry* shardRegistry) {
    vector<ShardId> allShardIds;

    shardRegistry->getAllShardIds(&allShardIds);
    if (allShardIds.empty()) {
        shardRegistry->reload(txn);
        shardRegistry->getAllShardIds(&allShardIds);

        if (allShardIds.empty()) {
            return Status(ErrorCodes::ShardNotFound, "No shards found");
        }
    }

    ShardId candidateShardId = allShardIds[0];

    auto candidateSizeStatus = shardutil::retrieveTotalShardSize(txn, candidateShardId);
    if (!candidateSizeStatus.isOK()) {
        return candidateSizeStatus.getStatus();
    }

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const ShardId shardId = allShardIds[i];

        const auto sizeStatus = shardutil::retrieveTotalShardSize(txn, shardId);
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

Status CatalogManagerReplicaSet::enableSharding(OperationContext* txn, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    DatabaseType db;

    // Lock the database globally to prevent conflicts with simultaneous database
    // creation/modification.
    auto scopedDistLock = getDistLockManager()->lock(txn, dbName, "enableSharding");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // Check for case sensitivity violations
    Status status = _checkDbDoesNotExist(txn, dbName, &db);
    if (status.isOK()) {
        // Database does not exist, create a new entry
        auto newShardIdStatus = _selectShardForNewDatabase(txn, grid.shardRegistry());
        if (!newShardIdStatus.isOK()) {
            return newShardIdStatus.getStatus();
        }

        const ShardId& newShardId = newShardIdStatus.getValue();

        log() << "Placing [" << dbName << "] on: " << newShardId;

        db.setName(dbName);
        db.setPrimary(newShardId);
        db.setSharded(true);
    } else if (status.code() == ErrorCodes::NamespaceExists) {
        if (db.getSharded()) {
            return Status(ErrorCodes::AlreadyInitialized,
                          str::stream() << "sharding already enabled for database " << dbName);
        }

        // Database exists, so just update it
        db.setSharded(true);
    } else {
        return status;
    }

    log() << "Enabling sharding for database [" << dbName << "] in config db";

    return updateDatabase(txn, dbName, db);
}

Status CatalogManagerReplicaSet::_log(OperationContext* txn,
                                      const StringData& logCollName,
                                      const std::string& what,
                                      const std::string& operationNS,
                                      const BSONObj& detail) {
    Date_t now = grid.shardRegistry()->getExecutor()->now();
    const std::string hostName = grid.shardRegistry()->getNetwork()->getHostName();
    const string changeId = str::stream() << hostName << "-" << now.toString() << "-" << OID::gen();

    ChangeLogType changeLog;
    changeLog.setChangeId(changeId);
    changeLog.setServer(hostName);
    changeLog.setClientAddr(txn->getClient()->clientAddress(true));
    changeLog.setTime(now);
    changeLog.setNS(operationNS);
    changeLog.setWhat(what);
    changeLog.setDetails(detail);

    BSONObj changeLogBSON = changeLog.toBSON();
    log() << "about to log metadata event into " << logCollName << ": " << changeLogBSON;

    const NamespaceString nss("config", logCollName);
    Status result = insertConfigDocument(txn, nss.ns(), changeLogBSON);
    if (!result.isOK()) {
        warning() << "Error encountered while logging config change with ID [" << changeId
                  << "] into collection " << logCollName << ": " << result;
    }

    return result;
}

StatusWith<DistLockManager::ScopedDistLock> CatalogManagerReplicaSet::distLock(
    OperationContext* txn,
    StringData name,
    StringData whyMessage,
    stdx::chrono::milliseconds waitFor) {
    return getDistLockManager()->lock(txn, name, whyMessage, waitFor);
}

Status CatalogManagerReplicaSet::shardCollection(OperationContext* txn,
                                                 const string& ns,
                                                 const ShardKeyPattern& fieldsAndOrder,
                                                 bool unique,
                                                 const vector<BSONObj>& initPoints,
                                                 const set<ShardId>& initShardIds) {
    // Lock the collection globally so that no other mongos can try to shard or drop the collection
    // at the same time.
    auto scopedDistLock = getDistLockManager()->lock(txn, ns, "shardCollection");
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    auto status = getDatabase(txn, nsToDatabase(ns));
    if (!status.isOK()) {
        return status.getStatus();
    }

    ShardId dbPrimaryShardId = status.getValue().value.getPrimary();
    const auto primaryShard = grid.shardRegistry()->getShard(txn, dbPrimaryShardId);

    {
        // In 3.0 and prior we include this extra safety check that the collection is not getting
        // sharded concurrently by two different mongos instances. It is not 100%-proof, but it
        // reduces the chance that two invocations of shard collection will step on each other's
        // toes.  Now we take the distributed lock so going forward this check won't be necessary
        // but we leave it around for compatibility with other mongoses from 3.0.
        // TODO(spencer): Remove this after 3.2 ships.
        auto countStatus = _runCountCommandOnConfig(
            txn, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::ns(ns)));
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

        logChange(txn, "shardCollection.start", ns, collectionDetail.obj());
    }

    shared_ptr<ChunkManager> manager(new ChunkManager(ns, fieldsAndOrder, unique));
    Status createFirstChunksStatus =
        manager->createFirstChunks(txn, dbPrimaryShardId, &initPoints, &initShardIds);
    if (!createFirstChunksStatus.isOK()) {
        return createFirstChunksStatus;
    }
    manager->loadExistingRanges(txn, nullptr);

    CollectionInfo collInfo;
    collInfo.useChunkManager(manager);
    collInfo.save(txn, ns);

    // Tell the primary mongod to refresh its data
    // TODO:  Think the real fix here is for mongos to just
    //        assume that all collections are sharded, when we get there
    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        grid.shardRegistry()->getConfigServerConnectionString(),
        dbPrimaryShardId,
        primaryShard->getConnString(),
        NamespaceString(ns),
        manager->getVersion(),
        true);

    auto ssvStatus = grid.shardRegistry()->runIdempotentCommandOnShard(
        txn,
        dbPrimaryShardId,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        ssv.toBSON());
    if (!ssvStatus.isOK()) {
        warning() << "could not update initial version of " << ns << " on shard primary "
                  << dbPrimaryShardId << ssvStatus.getStatus();
    }

    logChange(txn, "shardCollection.end", ns, BSON("version" << manager->getVersion().toString()));

    return Status::OK();
}

StatusWith<ShardDrainingStatus> CatalogManagerReplicaSet::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
    // Check preconditions for removing the shard
    auto countStatus = _runCountCommandOnConfig(
        txn,
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
        txn, NamespaceString(ShardType::ConfigNS), BSON(ShardType::name() << NE << name));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    if (countStatus.getValue() == 0) {
        return Status(ErrorCodes::IllegalOperation, "Can't remove last shard");
    }

    // Figure out if shard is already draining
    countStatus =
        _runCountCommandOnConfig(txn,
                                 NamespaceString(ShardType::ConfigNS),
                                 BSON(ShardType::name() << name << ShardType::draining(true)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }

    if (countStatus.getValue() == 0) {
        log() << "going to start draining shard: " << name;

        auto updateStatus = updateConfigDocument(txn,
                                                 ShardType::ConfigNS,
                                                 BSON(ShardType::name() << name),
                                                 BSON("$set" << BSON(ShardType::draining(true))),
                                                 false);
        if (!updateStatus.isOK()) {
            log() << "error starting removeShard: " << name << causedBy(updateStatus.getStatus());
            return updateStatus.getStatus();
        }

        grid.shardRegistry()->reload(txn);

        // Record start in changelog
        logChange(txn, "removeShard.start", "", BSON("shard" << name));
        return ShardDrainingStatus::STARTED;
    }

    // Draining has already started, now figure out how many chunks and databases are still on the
    // shard.
    countStatus = _runCountCommandOnConfig(
        txn, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(name)));
    if (!countStatus.isOK()) {
        return countStatus.getStatus();
    }
    const long long chunkCount = countStatus.getValue();

    countStatus = _runCountCommandOnConfig(
        txn, NamespaceString(DatabaseType::ConfigNS), BSON(DatabaseType::primary(name)));
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

    Status status =
        removeConfigDocuments(txn, ShardType::ConfigNS, BSON(ShardType::name() << name));
    if (!status.isOK()) {
        log() << "Error concluding removeShard operation on: " << name
              << "; err: " << status.reason();
        return status;
    }

    grid.shardRegistry()->remove(name);
    grid.shardRegistry()->reload(txn);

    // Record finish in changelog
    logChange(txn, "removeShard", "", BSON("shard" << name));

    return ShardDrainingStatus::COMPLETED;
}

StatusWith<repl::OpTimeWith<DatabaseType>> CatalogManagerReplicaSet::getDatabase(
    OperationContext* txn, const std::string& dbName) {
    if (!NamespaceString::validDBName(dbName, NamespaceString::DollarInDbNameBehavior::Allow)) {
        return {ErrorCodes::InvalidNamespace, stream() << dbName << " is not a valid db name"};
    }

    // The two databases that are hosted on the config server are config and admin
    if (dbName == "config" || dbName == "admin") {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setSharded(false);
        dbt.setPrimary("config");

        return repl::OpTimeWith<DatabaseType>(dbt);
    }

    auto result = _fetchDatabaseMetadata(txn, dbName, kConfigReadSelector);
    if (result == ErrorCodes::NamespaceNotFound) {
        // If we failed to find the database metadata on the 'nearest' config server, try again
        // against the primary, in case the database was recently created.
        result =
            _fetchDatabaseMetadata(txn, dbName, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
        if (!result.isOK() && (result != ErrorCodes::NamespaceNotFound)) {
            return Status(result.getStatus().code(),
                          str::stream() << "Could not confirm non-existence of database \""
                                        << dbName
                                        << "\" due to inability to query the config server primary"
                                        << causedBy(result.getStatus()));
        }
    }

    return result;
}

StatusWith<repl::OpTimeWith<DatabaseType>> CatalogManagerReplicaSet::_fetchDatabaseMetadata(
    OperationContext* txn, const std::string& dbName, const ReadPreferenceSetting& readPref) {
    dassert(dbName != "admin" && dbName != "config");

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              readPref,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              BSON(DatabaseType::name(dbName)),
                                              BSONObj(),
                                              1);
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

StatusWith<repl::OpTimeWith<CollectionType>> CatalogManagerReplicaSet::getCollection(
    OperationContext* txn, const std::string& collNs) {
    auto statusFind = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
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

    return repl::OpTimeWith<CollectionType>(parseStatus.getValue(), retOpTimePair.opTime);
}

Status CatalogManagerReplicaSet::getCollections(OperationContext* txn,
                                                const std::string* dbName,
                                                std::vector<CollectionType>* collections,
                                                OpTime* opTime) {
    BSONObjBuilder b;
    if (dbName) {
        invariant(!dbName->empty());
        b.appendRegex(CollectionType::fullNs(),
                      string(str::stream() << "^" << pcrecpp::RE::QuoteMeta(*dbName) << "\\."));
    }

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
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
                                  << " document: " << obj << " : "
                                  << collectionResult.getStatus().toString()};
        }

        collections->push_back(collectionResult.getValue());
    }

    if (opTime) {
        *opTime = docsOpTimePair.opTime;
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::dropCollection(OperationContext* txn, const NamespaceString& ns) {
    logChange(txn, "dropCollection.start", ns.ns(), BSONObj());

    auto shardsStatus = getAllShards(txn);
    if (!shardsStatus.isOK()) {
        return shardsStatus.getStatus();
    }
    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    LOG(1) << "dropCollection " << ns << " started";

    // Lock the collection globally so that split/migrate cannot run
    stdx::chrono::seconds waitFor(DistLockManager::kDefaultLockTimeout);
    MONGO_FAIL_POINT_BLOCK(setDropCollDistLockWait, customWait) {
        const BSONObj& data = customWait.getData();
        waitFor = stdx::chrono::seconds(data["waitForSecs"].numberInt());
    }

    auto scopedDistLock = getDistLockManager()->lock(txn, ns.ns(), "drop", waitFor);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    LOG(1) << "dropCollection " << ns << " locked";

    std::map<string, BSONObj> errors;
    auto* shardRegistry = grid.shardRegistry();

    for (const auto& shardEntry : allShards) {
        auto dropResult = shardRegistry->runIdempotentCommandOnShard(
            txn,
            shardEntry.getName(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            ns.db().toString(),
            BSON("drop" << ns.coll() << "writeConcern" << txn->getWriteConcern().toBSON()));

        if (!dropResult.isOK()) {
            return Status(dropResult.getStatus().code(),
                          dropResult.getStatus().reason() + " at " + shardEntry.getName());
        }

        auto dropStatus = getStatusFromCommandResult(dropResult.getValue());
        if (!dropStatus.isOK()) {
            if (dropStatus.code() == ErrorCodes::NamespaceNotFound) {
                continue;
            }

            errors.emplace(shardEntry.getHost(), dropResult.getValue());
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
    Status result = removeConfigDocuments(txn, ChunkType::ConfigNS, BSON(ChunkType::ns(ns.ns())));
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << ns << " chunk data deleted";

    // Mark the collection as dropped
    CollectionType coll;
    coll.setNs(ns);
    coll.setDropped(true);
    coll.setEpoch(ChunkVersion::DROPPED().epoch());
    coll.setUpdatedAt(grid.shardRegistry()->getNetwork()->now());

    result = updateCollection(txn, ns.ns(), coll);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << ns << " collection marked as dropped";

    for (const auto& shardEntry : allShards) {
        SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
            grid.shardRegistry()->getConfigServerConnectionString(),
            shardEntry.getName(),
            fassertStatusOK(28781, ConnectionString::parse(shardEntry.getHost())),
            ns,
            ChunkVersion::DROPPED(),
            true);

        auto ssvResult = shardRegistry->runIdempotentCommandOnShard(
            txn,
            shardEntry.getName(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            ssv.toBSON());

        if (!ssvResult.isOK()) {
            return ssvResult.getStatus();
        }

        auto ssvStatus = getStatusFromCommandResult(ssvResult.getValue());
        if (!ssvStatus.isOK()) {
            return ssvStatus;
        }

        auto unsetShardingStatus = shardRegistry->runIdempotentCommandOnShard(
            txn,
            shardEntry.getName(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON("unsetSharding" << 1));

        if (!unsetShardingStatus.isOK()) {
            return unsetShardingStatus.getStatus();
        }

        auto unsetShardingResult = getStatusFromCommandResult(unsetShardingStatus.getValue());
        if (!unsetShardingResult.isOK()) {
            return unsetShardingResult;
        }
    }

    LOG(1) << "dropCollection " << ns << " completed";

    logChange(txn, "dropCollection", ns.ns(), BSONObj());

    return Status::OK();
}

StatusWith<SettingsType> CatalogManagerReplicaSet::getGlobalSettings(OperationContext* txn,
                                                                     const string& key) {
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(SettingsType::ConfigNS),
                                              BSON(SettingsType::key(key)),
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

Status CatalogManagerReplicaSet::getDatabasesForShard(OperationContext* txn,
                                                      const string& shardName,
                                                      vector<string>* dbs) {
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(DatabaseType::ConfigNS),
                                              BSON(DatabaseType::primary(shardName)),
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

Status CatalogManagerReplicaSet::getChunks(OperationContext* txn,
                                           const BSONObj& query,
                                           const BSONObj& sort,
                                           boost::optional<int> limit,
                                           vector<ChunkType>* chunks,
                                           OpTime* opTime) {
    chunks->clear();

    // Convert boost::optional<int> to boost::optional<long long>.
    auto longLimit = limit ? boost::optional<long long>(*limit) : boost::none;
    auto findStatus = _exhaustiveFindOnConfig(
        txn, kConfigReadSelector, NamespaceString(ChunkType::ConfigNS), query, sort, longLimit);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto chunkDocsOpTimePair = findStatus.getValue();
    for (const BSONObj& obj : chunkDocsOpTimePair.value) {
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

    if (opTime) {
        *opTime = chunkDocsOpTimePair.opTime;
    }

    return Status::OK();
}

Status CatalogManagerReplicaSet::getTagsForCollection(OperationContext* txn,
                                                      const std::string& collectionNs,
                                                      std::vector<TagsType>* tags) {
    tags->clear();

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(TagsType::ConfigNS),
                                              BSON(TagsType::ns(collectionNs)),
                                              BSON(TagsType::min() << 1),
                                              boost::none);  // no limit
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }
    for (const BSONObj& obj : findStatus.getValue().value) {
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

StatusWith<string> CatalogManagerReplicaSet::getTagForChunk(OperationContext* txn,
                                                            const std::string& collectionNs,
                                                            const ChunkType& chunk) {
    BSONObj query =
        BSON(TagsType::ns(collectionNs) << TagsType::min() << BSON("$lte" << chunk.getMin())
                                        << TagsType::max() << BSON("$gte" << chunk.getMax()));
    auto findStatus = _exhaustiveFindOnConfig(
        txn, kConfigReadSelector, NamespaceString(TagsType::ConfigNS), query, BSONObj(), 1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
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

StatusWith<repl::OpTimeWith<std::vector<ShardType>>> CatalogManagerReplicaSet::getAllShards(
    OperationContext* txn) {
    std::vector<ShardType> shards;
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
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
            shards.clear();
            return {ErrorCodes::FailedToParse,
                    stream() << "Failed to parse shard with id ("
                             << doc[ShardType::name()].toString() << ")"
                             << causedBy(shardRes.getStatus())};
        }

        Status validateStatus = shardRes.getValue().validate();
        if (!validateStatus.isOK()) {
            return {validateStatus.code(),
                    stream() << "Failed to validate shard with id ("
                             << doc[ShardType::name()].toString() << ")"
                             << causedBy(validateStatus)};
        }

        shards.push_back(shardRes.getValue());
    }

    return repl::OpTimeWith<std::vector<ShardType>>{std::move(shards),
                                                    findStatus.getValue().opTime};
}

bool CatalogManagerReplicaSet::runUserManagementWriteCommand(OperationContext* txn,
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
        const char* writeConcernFieldName = "writeConcern";
        BSONElement writeConcernElement = cmdObj[writeConcernFieldName];
        bool initialCmdHadWriteConcern = !writeConcernElement.eoo();
        if (initialCmdHadWriteConcern) {
            Status status = writeConcern.parse(writeConcernElement.Obj());
            if (!status.isOK()) {
                return Command::appendCommandStatus(*result, status);
            }
            if (!writeConcern.validForConfigServers()) {
                return Command::appendCommandStatus(
                    *result,
                    Status(ErrorCodes::InvalidOptions,
                           str::stream()
                               << "Invalid replication write concern.  Writes to config server "
                                  "replica sets must use w:'majority', got: "
                               << writeConcern.toBSON()));
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
                if (str::equals(e.fieldName(), writeConcernFieldName)) {
                    continue;
                }
                modifiedCmd.append(e);
            }
        }
        modifiedCmd.append(writeConcernFieldName, writeConcern.toBSON());
        cmdToRun = modifiedCmd.obj();
    }

    auto response = grid.shardRegistry()->runCommandOnConfigWithRetries(
        txn, dbname, cmdToRun, ShardRegistry::kNotMasterErrors);

    if (!response.isOK()) {
        return Command::appendCommandStatus(*result, response.getStatus());
    }

    result->appendElements(response.getValue());
    return getStatusFromCommandResult(response.getValue()).isOK();
}

bool CatalogManagerReplicaSet::runReadCommandForTest(OperationContext* txn,
                                                     const std::string& dbname,
                                                     const BSONObj& cmdObj,
                                                     BSONObjBuilder* result) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.appendElements(cmdObj);
    _appendReadConcern(&cmdBuilder);

    auto resultStatus = grid.shardRegistry()->runIdempotentCommandOnConfig(
        txn, kConfigReadSelector, dbname, cmdBuilder.done());
    if (resultStatus.isOK()) {
        result->appendElements(resultStatus.getValue());
        return getStatusFromCommandResult(resultStatus.getValue()).isOK();
    }

    return Command::appendCommandStatus(*result, resultStatus.getStatus());
}

bool CatalogManagerReplicaSet::runUserManagementReadCommand(OperationContext* txn,
                                                            const std::string& dbname,
                                                            const BSONObj& cmdObj,
                                                            BSONObjBuilder* result) {
    auto resultStatus = grid.shardRegistry()->runIdempotentCommandOnConfig(
        txn, kConfigPrimaryPreferredSelector, dbname, cmdObj);
    if (resultStatus.isOK()) {
        result->appendElements(resultStatus.getValue());
        return getStatusFromCommandResult(resultStatus.getValue()).isOK();
    }

    return Command::appendCommandStatus(*result, resultStatus.getStatus());
}

Status CatalogManagerReplicaSet::applyChunkOpsDeprecated(OperationContext* txn,
                                                         const BSONArray& updateOps,
                                                         const BSONArray& preCondition,
                                                         const std::string& nss,
                                                         const ChunkVersion& lastChunkVersion) {
    BSONObj cmd = BSON("applyOps" << updateOps << "preCondition" << preCondition
                                  << kWriteConcernField << kMajorityWriteConcern.toBSON());

    auto response = grid.shardRegistry()->runCommandOnConfigWithRetries(
        txn, "config", cmd, ShardRegistry::kAllRetriableErrors);

    if (!response.isOK()) {
        return response.getStatus();
    }

    Status status = getStatusFromCommandResult(response.getValue());

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
                  << causedBy(status);

        // Look for the chunk in this shard whose version got bumped. We assume that if that
        // mod made it to the config server, then applyOps was successful.
        std::vector<ChunkType> newestChunk;
        BSONObjBuilder query;
        lastChunkVersion.addToBSON(query, ChunkType::DEPRECATED_lastmod());
        query.append(ChunkType::ns(), nss);
        Status chunkStatus = getChunks(txn, query.obj(), BSONObj(), 1, &newestChunk, nullptr);

        if (!chunkStatus.isOK()) {
            warning() << "getChunks function failed, unable to validate chunk operation metadata"
                      << causedBy(chunkStatus);
            errMsg = str::stream() << "getChunks function failed, unable to validate chunk "
                                   << "operation metadata: " << causedBy(chunkStatus)
                                   << ". applyChunkOpsDeprecated failed to get confirmation "
                                   << "of commit. Unable to save chunk ops. Command: " << cmd
                                   << ". Result: " << response.getValue();
        } else if (!newestChunk.empty()) {
            invariant(newestChunk.size() == 1);
            log() << "chunk operation commit confirmed";
            return Status::OK();
        } else {
            errMsg = str::stream() << "chunk operation commit failed: version "
                                   << lastChunkVersion.toString() << " doesn't exist in namespace"
                                   << nss << ". Unable to save chunk ops. Command: " << cmd
                                   << ". Result: " << response.getValue();
        }
        return Status(status.code(), errMsg);
    }

    return Status::OK();
}

DistLockManager* CatalogManagerReplicaSet::getDistLockManager() {
    invariant(_distLockManager);
    return _distLockManager.get();
}

void CatalogManagerReplicaSet::writeConfigServerDirect(OperationContext* txn,
                                                       const BatchedCommandRequest& batchRequest,
                                                       BatchedCommandResponse* batchResponse) {
    // We only support batch sizes of one for config writes
    if (batchRequest.sizeWriteOps() != 1) {
        toBatchError(Status(ErrorCodes::InvalidOptions,
                            str::stream() << "Writes to config servers must have batch size of 1, "
                                          << "found " << batchRequest.sizeWriteOps()),
                     batchResponse);
        return;
    }

    _runBatchWriteCommand(txn, batchRequest, batchResponse, ShardRegistry::kNotMasterErrors);
}

void CatalogManagerReplicaSet::_runBatchWriteCommand(
    OperationContext* txn,
    const BatchedCommandRequest& batchRequest,
    BatchedCommandResponse* batchResponse,
    const ShardRegistry::ErrorCodesSet& errorsToCheck) {
    const std::string dbname = batchRequest.getNS().db().toString();
    invariant(dbname == "config" || dbname == "admin");

    invariant(batchRequest.sizeWriteOps() == 1);

    const BSONObj cmdObj = batchRequest.toBSON();

    for (int retry = 1; retry <= kMaxWriteRetry; ++retry) {
        // runCommandOnConfigWithRetries already does its own retries based on the generic command
        // errors. If this fails, we know that it has done all the retries that it could do so there
        // is no need to retry anymore.
        auto response =
            grid.shardRegistry()->runCommandOnConfigWithRetries(txn, dbname, cmdObj, errorsToCheck);
        if (!response.isOK()) {
            toBatchError(response.getStatus(), batchResponse);
            return;
        }

        string errmsg;
        if (!batchResponse->parseBSON(response.getValue(), &errmsg)) {
            toBatchError(
                Status(ErrorCodes::FailedToParse,
                       str::stream() << "Failed to parse config server response: " << errmsg),
                batchResponse);
            return;
        }

        // If one of the write operations failed (which is reported in the error details), see if
        // this is a retriable error as well.
        Status status = batchResponse->toStatus();
        if (errorsToCheck.count(status.code()) && retry < kMaxWriteRetry) {
            LOG(1) << "Command failed with retriable error and will be retried" << causedBy(status);
            continue;
        }

        return;
    }

    MONGO_UNREACHABLE;
}

Status CatalogManagerReplicaSet::insertConfigDocument(OperationContext* txn,
                                                      const std::string& ns,
                                                      const BSONObj& doc) {
    const NamespaceString nss(ns);
    invariant(nss.db() == "config");

    const BSONElement idField = doc.getField("_id");
    invariant(!idField.eoo());

    auto insert(stdx::make_unique<BatchedInsertRequest>());
    insert->addToDocuments(doc);

    BatchedCommandRequest request(insert.release());
    request.setNS(nss);
    request.setWriteConcern(kMajorityWriteConcern.toBSON());

    for (int retry = 1; retry <= kMaxWriteRetry; retry++) {
        BatchedCommandResponse response;
        _runBatchWriteCommand(txn, request, &response, ShardRegistry::kNotMasterErrors);

        Status status = response.toStatus();

        // If we get DuplicateKey error on the first attempt to insert, this definitively means that
        // we are trying to insert the same entry a second time, so error out. If it happens on a
        // retry attempt though, it is not clear whether we are actually inserting a duplicate key
        // or it is because we failed to wait for write concern on the first attempt. In order to
        // differentiate, fetch the entry and check.
        if (retry > 1 && status == ErrorCodes::DuplicateKey) {
            LOG(1) << "Insert retry failed because of duplicate key error, rechecking.";

            auto fetchDuplicate =
                _exhaustiveFindOnConfig(txn,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        nss,
                                        idField.wrap(),
                                        BSONObj(),
                                        boost::none);
            if (!fetchDuplicate.isOK()) {
                return fetchDuplicate.getStatus();
            }

            auto existingDocs = fetchDuplicate.getValue().value;
            if (existingDocs.empty()) {
                return Status(ErrorCodes::DuplicateKey,
                              stream() << "DuplicateKey error" << causedBy(status)
                                       << " was returned after a retry attempt, but no documents "
                                          "were found. This means a concurrent change occurred "
                                          "together with the retries.");
            }

            invariant(existingDocs.size() == 1);

            BSONObj existing = std::move(existingDocs.front());
            if (existing.woCompare(doc) == 0) {
                // Documents match, so treat the operation as success
                return Status::OK();
            }
        }

        if (ShardRegistry::kAllRetriableErrors.count(status.code()) && (retry < kMaxWriteRetry)) {
            continue;
        }

        return status;
    }

    MONGO_UNREACHABLE;
}

StatusWith<bool> CatalogManagerReplicaSet::updateConfigDocument(OperationContext* txn,
                                                                const string& ns,
                                                                const BSONObj& query,
                                                                const BSONObj& update,
                                                                bool upsert) {
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
    request.setWriteConcern(kMajorityWriteConcern.toBSON());

    BatchedCommandResponse response;
    _runBatchWriteCommand(txn, request, &response, ShardRegistry::kAllRetriableErrors);

    Status status = response.toStatus();
    if (!status.isOK()) {
        return status;
    }

    const auto nSelected = response.getN();
    invariant(nSelected == 0 || nSelected == 1);
    return (nSelected == 1);
}

Status CatalogManagerReplicaSet::removeConfigDocuments(OperationContext* txn,
                                                       const string& ns,
                                                       const BSONObj& query) {
    const NamespaceString nss(ns);
    invariant(nss.db() == "config");

    auto deleteDoc(stdx::make_unique<BatchedDeleteDocument>());
    deleteDoc->setQuery(query);
    deleteDoc->setLimit(0);

    auto deleteRequest(stdx::make_unique<BatchedDeleteRequest>());
    deleteRequest->addToDeletes(deleteDoc.release());

    BatchedCommandRequest request(deleteRequest.release());
    request.setNS(nss);
    request.setWriteConcern(kMajorityWriteConcern.toBSON());

    BatchedCommandResponse response;
    _runBatchWriteCommand(txn, request, &response, ShardRegistry::kAllRetriableErrors);

    return response.toStatus();
}

Status CatalogManagerReplicaSet::_checkDbDoesNotExist(OperationContext* txn,
                                                      const string& dbName,
                                                      DatabaseType* db) {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
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
                                << " have: " << actualDbName << " want to add: " << dbName);
}

StatusWith<std::string> CatalogManagerReplicaSet::_generateNewShardName(OperationContext* txn) {
    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(ShardType::ConfigNS),
                                              shardNameRegex.obj(),
                                              BSON(ShardType::name() << -1),
                                              1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;

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

Status CatalogManagerReplicaSet::_createCappedConfigCollection(OperationContext* txn,
                                                               StringData collName,
                                                               int cappedSize) {
    BSONObj createCmd = BSON("create" << collName << "capped" << true << "size" << cappedSize);
    auto result = grid.shardRegistry()->runCommandOnConfigWithRetries(
        txn, "config", createCmd, ShardRegistry::kAllRetriableErrors);
    if (!result.isOK()) {
        return result.getStatus();
    }

    return getStatusFromCommandResult(result.getValue());
}

StatusWith<long long> CatalogManagerReplicaSet::_runCountCommandOnConfig(OperationContext* txn,
                                                                         const NamespaceString& ns,
                                                                         BSONObj query) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", ns.coll());
    countBuilder.append("query", query);
    _appendReadConcern(&countBuilder);

    auto resultStatus = grid.shardRegistry()->runIdempotentCommandOnConfig(
        txn, kConfigReadSelector, ns.db().toString(), countBuilder.done());
    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    auto responseObj = std::move(resultStatus.getValue());

    long long result;
    auto status = bsonExtractIntegerField(responseObj, "n", &result);
    if (!status.isOK()) {
        return status;
    }

    return result;
}

Status CatalogManagerReplicaSet::initConfigVersion(OperationContext* txn) {
    for (int x = 0; x < kMaxConfigVersionInitRetry; x++) {
        auto versionStatus = _getConfigVersion(txn);
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
            newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
            newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

            BSONObj versionObj(newVersion.toBSON());
            auto upsertStatus =
                updateConfigDocument(txn, VersionType::ConfigNS, versionObj, versionObj, true);

            if ((upsertStatus.isOK() && !upsertStatus.getValue()) ||
                upsertStatus == ErrorCodes::DuplicateKey) {
                // Do the check again as someone inserted a new config version document
                // and the upsert neither inserted nor updated a config version document.
                // Note: you can get duplicate key errors on upsert because of SERVER-14322.
                continue;
            }

            return upsertStatus.getStatus();
        }

        if (versionInfo.getCurrentVersion() == UpgradeHistory_UnreportedVersion) {
            return {ErrorCodes::IncompatibleShardingConfigVersion,
                    "Assuming config data is old since the version document cannot be found in the "
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

    return {ErrorCodes::IncompatibleShardingConfigVersion,
            str::stream() << "unable to create new config version document after "
                          << kMaxConfigVersionInitRetry << " retries"};
}

StatusWith<VersionType> CatalogManagerReplicaSet::_getConfigVersion(OperationContext* txn) {
    auto findStatus = _exhaustiveFindOnConfig(txn,
                                              kConfigReadSelector,
                                              NamespaceString(VersionType::ConfigNS),
                                              BSONObj(),
                                              BSONObj(),
                                              boost::none /* no limit */);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    auto queryResults = findStatus.getValue().value;

    if (queryResults.size() > 1) {
        return {ErrorCodes::RemoteValidationError,
                str::stream() << "should only have 1 document in " << VersionType::ConfigNS};
    }

    if (queryResults.empty()) {
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

StatusWith<repl::OpTimeWith<vector<BSONObj>>> CatalogManagerReplicaSet::_exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto response =
        grid.shardRegistry()->exhaustiveFindOnConfig(txn, readPref, nss, query, sort, limit);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return repl::OpTimeWith<vector<BSONObj>>(std::move(response.getValue().docs),
                                             response.getValue().opTime);
}

void CatalogManagerReplicaSet::_appendReadConcern(BSONObjBuilder* builder) {
    repl::ReadConcernArgs readConcern(grid.configOpTime(),
                                      repl::ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(builder);
}

Status CatalogManagerReplicaSet::appendInfoForConfigServerDatabases(OperationContext* txn,
                                                                    BSONArrayBuilder* builder) {
    auto resultStatus = grid.shardRegistry()->runIdempotentCommandOnConfig(
        txn, kConfigPrimaryPreferredSelector, "admin", BSON("listDatabases" << 1));

    if (!resultStatus.isOK()) {
        return resultStatus.getStatus();
    }

    auto listDBResponse = resultStatus.getValue();
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

}  // namespace mongo
