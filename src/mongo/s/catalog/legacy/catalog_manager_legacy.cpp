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

#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"

#include <iomanip>
#include <map>
#include <memory>
#include <pcrecpp.h>
#include <set>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/legacy/cluster_client_internal.h"
#include "mongo/s/catalog/legacy/config_coordinator.h"
#include "mongo/s/catalog/type_actionlog.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/dbclient_multi_command.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/config.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/legacy/legacy_dist_lock_manager.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using std::map;
    using std::pair;
    using std::set;
    using std::string;
    using std::vector;
    using str::stream;

namespace {

    bool validConfigWC(const BSONObj& writeConcern) {
        BSONElement elem(writeConcern["w"]);
        if (elem.eoo()) {
            return true;
        }

        if (elem.isNumber() && elem.numberInt() <= 1) {
            return true;
        }

        if (elem.type() == String && elem.str() == "majority") {
            return true;
        }

        return false;
    }

    void toBatchError(const Status& status, BatchedCommandResponse* response) {
        response->clear();
        response->setErrCode(status.code());
        response->setErrMessage(status.reason());
        response->setOk(false);

        dassert(response->isValid(NULL));
    }

    StatusWith<string> isValidShard(const string& name,
                                    const ConnectionString& shardConnectionString,
                                    ScopedDbConnection& conn) {
        if (conn->type() == ConnectionString::SYNC) {
            return Status(ErrorCodes::BadValue,
                          "can't use sync cluster as a shard; for a replica set, "
                          "you have to use <setname>/<server1>,<server2>,...");
        }

        BSONObj resIsMongos;
        // (ok == 0) implies that it is a mongos
        if (conn->runCommand("admin", BSON("isdbgrid" << 1), resIsMongos)) {
            return Status(ErrorCodes::BadValue,
                          "can't add a mongos process as a shard");
        }

        BSONObj resIsMaster;
        if (!conn->runCommand("admin", BSON("isMaster" << 1), resIsMaster)) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "failed running isMaster: " << resIsMaster);
        }

        // if the shard has only one host, make sure it is not part of a replica set
        string setName = resIsMaster["setName"].str();
        string commandSetName = shardConnectionString.getSetName();
        if (commandSetName.empty() && !setName.empty()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "host is part of set " << setName << "; "
                                        << "use replica set url format "
                                        << "<setname>/<server1>,<server2>, ...");
        }

        if (!commandSetName.empty() && setName.empty()) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "host did not return a set name; "
                                        << "is the replica set still initializing? "
                                        << resIsMaster);
        }

        // if the shard is part of replica set, make sure it is the right one
        if (!commandSetName.empty() && (commandSetName != setName)) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "host is part of a different set: " << setName);
        }

        if (setName.empty()) {
            // check this isn't a --configsvr
            BSONObj res;
            bool ok = conn->runCommand("admin",
                                       BSON("replSetGetStatus" << 1),
                                       res);
            if(!ok && res["info"].type() == String && res["info"].String() == "configsvr") {
                return Status(ErrorCodes::BadValue,
                              "the specified mongod is a --configsvr and "
                              "should thus not be a shard server");
            }
        }

        // if the shard is part of a replica set,
        // make sure all the hosts mentioned in 'shardConnectionString' are part of
        // the set. It is fine if not all members of the set are present in 'shardConnectionString'.
        bool foundAll = true;
        string offendingHost;
        if (!commandSetName.empty()) {
            set<string> hostSet;
            BSONObjIterator iter(resIsMaster["hosts"].Obj());
            while (iter.more()) {
                hostSet.insert(iter.next().String()); // host:port
            }
            if (resIsMaster["passives"].isABSONObj()) {
                BSONObjIterator piter(resIsMaster["passives"].Obj());
                while (piter.more()) {
                    hostSet.insert(piter.next().String()); // host:port
                }
            }
            if (resIsMaster["arbiters"].isABSONObj()) {
                BSONObjIterator piter(resIsMaster["arbiters"].Obj());
                while (piter.more()) {
                    hostSet.insert(piter.next().String()); // host:port
                }
            }

            vector<HostAndPort> hosts = shardConnectionString.getServers();
            for (size_t i = 0; i < hosts.size(); i++) {
                if (!hosts[i].hasPort()) {
                    hosts[i] = HostAndPort(hosts[i].host(), hosts[i].port());
                }
                string host = hosts[i].toString(); // host:port
                if (hostSet.find(host) == hostSet.end()) {
                    offendingHost = host;
                    foundAll = false;
                    break;
                }
            }
        }
        if (!foundAll) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "in seed list " << shardConnectionString.toString()
                                        << ", host " << offendingHost
                                        << " does not belong to replica set " << setName);
        }

        string shardName(name);
        // shard name defaults to the name of the replica set
        if (name.empty() && !setName.empty()) {
            shardName = setName;
        }

        // disallow adding shard replica set with name 'config'
        if (shardName == "config") {
            return Status(ErrorCodes::BadValue,
                          "use of shard replica set with name 'config' is not allowed");
        }

        return shardName;
    }

    // In order to be accepted as a new shard, that mongod must not have
    // any database name that exists already in any other shards.
    // If that test passes, the new shard's databases are going to be entered as
    // non-sharded db's whose primary is the newly added shard.
    StatusWith<vector<string>> getDBNames(const ConnectionString& shardConnectionString,
                                          ScopedDbConnection& conn) {
        vector<string> dbNames;

        BSONObj resListDB;
        if (!conn->runCommand("admin", BSON("listDatabases" << 1), resListDB)) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "failed listing "
                                        << shardConnectionString.toString()
                                        << "'s databases:" << resListDB);
        }

        BSONObjIterator i(resListDB["databases"].Obj());
        while (i.more()) {
            BSONObj dbEntry = i.next().Obj();
            const string& dbName = dbEntry["name"].String();
            if (!(dbName == "local" || dbName == "admin" || dbName == "config")) {
                dbNames.push_back(dbName);
            }
        }

        return dbNames;
    }

    BSONObj buildRemoveLogEntry(const string& shardName, bool isDraining) {
        BSONObjBuilder details;
        details.append("shard", shardName);
        details.append("isDraining", isDraining);

        return details.obj();
    }

    // Whether the logChange call should attempt to create the changelog collection
    AtomicInt32 changeLogCollectionCreated(0);

    // Whether the logAction call should attempt to create the actionlog collection
    AtomicInt32 actionLogCollectionCreated(0);

} // namespace


    CatalogManagerLegacy::CatalogManagerLegacy() = default;

    CatalogManagerLegacy::~CatalogManagerLegacy() = default;

    Status CatalogManagerLegacy::init(const ConnectionString& configDBCS) {
        // Initialization should not happen more than once
        invariant(!_configServerConnectionString.isValid());
        invariant(_configServers.empty());
        invariant(configDBCS.isValid());
        
        // Extract the hosts in HOST:PORT format
        set<HostAndPort> configHostsAndPortsSet;
        set<string> configHostsOnly;
        std::vector<HostAndPort> configHostAndPorts = configDBCS.getServers();
        for (size_t i = 0; i < configHostAndPorts.size(); i++) {
            // Append the default port, if not specified
            HostAndPort configHost = configHostAndPorts[i];
            if (!configHost.hasPort()) {
                configHost = HostAndPort(configHost.host(), ServerGlobalParams::ConfigServerPort);
            }

            // Make sure there are no duplicates
            if (!configHostsAndPortsSet.insert(configHost).second) {
                StringBuilder sb;
                sb << "Host " << configHost.toString()
                   << " exists twice in the config servers listing.";

                return Status(ErrorCodes::InvalidOptions, sb.str());
            }

            configHostsOnly.insert(configHost.host());
        }

        // Make sure the hosts are reachable
        for (set<string>::const_iterator i = configHostsOnly.begin();
             i != configHostsOnly.end();
             i++) {

            const string host = *i;

            // If this is a CUSTOM connection string (for testing) don't do DNS resolution
            string errMsg;
            if (ConnectionString::parse(host, errMsg).type() == ConnectionString::CUSTOM) {
                continue;
            }

            bool ok = false;

            for (int x = 10; x > 0; x--) {
                if (!hostbyname(host.c_str()).empty()) {
                    ok = true;
                    break;
                }

                log() << "can't resolve DNS for [" << host << "]  sleeping and trying "
                      << x << " more times";
                sleepsecs(10);
            }

            if (!ok) {
                return Status(ErrorCodes::HostNotFound,
                              stream() << "unable to resolve DNS for host " << host);
            }
        }

        LOG(1) << " config string : " << configDBCS.toString();

        // Now that the config hosts are verified, initialize the catalog manager. The code below
        // should never fail.

        _configServerConnectionString = configDBCS;

        if (_configServerConnectionString.type() == ConnectionString::MASTER) {
            _configServers.push_back(_configServerConnectionString);
        }
        else if (_configServerConnectionString.type() == ConnectionString::SYNC ||
                 (_configServerConnectionString.type() == ConnectionString::SET &&
                         _configServerConnectionString.getServers().size() == 1)) {
            // TODO(spencer): Remove second part of the above or statement that allows replset
            // config server strings once we've separated the legacy catalog manager from the
            // CSRS version.
            const vector<HostAndPort> configHPs = _configServerConnectionString.getServers();
            for (vector<HostAndPort>::const_iterator it = configHPs.begin();
                 it != configHPs.end();
                 ++it) {

                _configServers.push_back(ConnectionString(*it));
            }
        }
        else {
            // This is only for tests.
            invariant(_configServerConnectionString.type() == ConnectionString::CUSTOM);
            _configServers.push_back(_configServerConnectionString);
        }

        _distLockManager = stdx::make_unique<LegacyDistLockManager>(_configServerConnectionString);
        _distLockManager->startUp();

        {
            boost::lock_guard<boost::mutex> lk(_mutex);
            _inShutdown = false;
            _consistentFromLastCheck = true;
        }

        return Status::OK();
    }

    Status CatalogManagerLegacy::startConfigServerChecker() {
        if (!_checkConfigServersConsistent()) {
            return Status(ErrorCodes::IncompatibleShardingMetadata,
                          "Data inconsistency detected amongst config servers");
        }

        boost::thread t(stdx::bind(&CatalogManagerLegacy::_consistencyChecker, this));
        _consistencyCheckerThread.swap(t);

        return Status::OK();
    }

    void CatalogManagerLegacy::shutDown() {
        LOG(1) << "CatalogManagerLegacy::shutDown() called.";
        {
            boost::lock_guard<boost::mutex> lk(_mutex);
            _inShutdown = true;
            _consistencyCheckerCV.notify_one();
        }
        _consistencyCheckerThread.join();

        invariant(_distLockManager);
        _distLockManager->shutDown();
    }

    Status CatalogManagerLegacy::enableSharding(const std::string& dbName) {
        invariant(nsIsDbOnly(dbName));

        DatabaseType db;

        // Check for case sensitivity violations
        Status status = _checkDbDoesNotExist(dbName);
        if (status.isOK()) {
            // Database does not exist, create a new entry
            const Shard primary = Shard::pick();
            if (primary.ok()) {
                log() << "Placing [" << dbName << "] on: " << primary;

                db.setName(dbName);
                db.setPrimary(primary.getName());
                db.setSharded(true);
            }
            else {
                return Status(ErrorCodes::ShardNotFound, "can't find a shard to put new db on");
            }
        }
        else if (status.code() == ErrorCodes::NamespaceExists) {
            // Database exists, so just update it
            StatusWith<DatabaseType> dbStatus = getDatabase(dbName);
            if (!dbStatus.isOK()) {
                return dbStatus.getStatus();
            }

            db = dbStatus.getValue();
            db.setSharded(true);
        }
        else {
            // Some fatal error
            return status;
        }

        log() << "Enabling sharding for database [" << dbName << "] in config db";

        return updateDatabase(dbName, db);
    }

    Status CatalogManagerLegacy::shardCollection(const string& ns,
                                                 const ShardKeyPattern& fieldsAndOrder,
                                                 bool unique,
                                                 vector<BSONObj>* initPoints,
                                                 vector<Shard>* initShards) {

        StatusWith<DatabaseType> status = getDatabase(nsToDatabase(ns));
        if (!status.isOK()) {
            return status.getStatus();
        }

        DatabaseType dbt = status.getValue();
        Shard dbPrimary = Shard::make(dbt.getPrimary());

        // This is an extra safety check that the collection is not getting sharded concurrently by
        // two different mongos instances. It is not 100%-proof, but it reduces the chance that two
        // invocations of shard collection will step on each other's toes.
        {
            ScopedDbConnection conn(_configServerConnectionString, 30);
            unsigned long long existingChunks = conn->count(ChunkType::ConfigNS,
                                                            BSON(ChunkType::ns(ns)));
            if (existingChunks > 0) {
                conn.done();
                return Status(ErrorCodes::AlreadyInitialized,
                    str::stream() << "collection " << ns << " already sharded with "
                                  << existingChunks << " chunks.");
            }

            conn.done();
        }

        log() << "enable sharding on: " << ns << " with shard key: " << fieldsAndOrder;

        // Record start in changelog
        BSONObjBuilder collectionDetail;
        collectionDetail.append("shardKey", fieldsAndOrder.toBSON());
        collectionDetail.append("collection", ns);
        collectionDetail.append("primary", dbPrimary.toString());

        BSONArray initialShards;
        if (initShards == NULL)
            initialShards = BSONArray();
        else {
            BSONArrayBuilder b;
            for (unsigned i = 0; i < initShards->size(); i++) {
                b.append((*initShards)[i].getName());
            }
            initialShards = b.arr();
        }

        collectionDetail.append("initShards", initialShards);
        collectionDetail.append("numChunks", static_cast<int>(initPoints->size() + 1));

        logChange(NULL, "shardCollection.start", ns, collectionDetail.obj());

        ChunkManagerPtr manager(new ChunkManager(ns, fieldsAndOrder, unique));
        manager->createFirstChunks(dbPrimary,
                                   initPoints,
                                   initShards);
        manager->loadExistingRanges(nullptr);

        CollectionInfo collInfo;
        collInfo.useChunkManager(manager);
        collInfo.save(ns);
        manager->reload(true);

        // Tell the primary mongod to refresh its data
        // TODO:  Think the real fix here is for mongos to just
        //        assume that all collections are sharded, when we get there
        for (int i = 0;i < 4;i++) {
            if (i == 3) {
                warning() << "too many tries updating initial version of " << ns
                          << " on shard primary " << dbPrimary
                          << ", other mongoses may not see the collection as sharded immediately";
                break;
            }

            try {
                ShardConnection conn(dbPrimary.getConnString(), ns);
                bool isVersionSet = conn.setVersion();
                conn.done();
                if (!isVersionSet) {
                    warning() << "could not update initial version of "
                              << ns << " on shard primary " << dbPrimary;
                } else {
                    break;
                }
            }
            catch (const DBException& e) {
                warning() << "could not update initial version of " << ns
                          << " on shard primary " << dbPrimary
                          << causedBy(e);
            }

            sleepsecs(i);
        }

        // Record finish in changelog
        BSONObjBuilder finishDetail;

        finishDetail.append("version", manager->getVersion().toString());

        logChange(NULL, "shardCollection", ns, finishDetail.obj());

        return Status::OK();
    }

    Status CatalogManagerLegacy::createDatabase(const std::string& dbName) {
        invariant(nsIsDbOnly(dbName));

        // The admin and config databases should never be explicitly created. They "just exist",
        // i.e. getDatabase will always return an entry for them.
        invariant(dbName != "admin");
        invariant(dbName != "config");

        // Lock the database globally to prevent conflicts with simultaneous database creation.
        auto scopedDistLock = getDistLockManager()->lock(dbName,
                                                         "createDatabase",
                                                         Seconds{5},
                                                         Milliseconds{500});
        if (!scopedDistLock.isOK()) {
            return scopedDistLock.getStatus();
        }

        // Check for case sensitivity violations
        auto status = _checkDbDoesNotExist(dbName);
        if (!status.isOK()) {
            return status;
        }

        // Database does not exist, pick a shard and create a new entry
        const Shard primaryShard = Shard::pick();
        if (!primaryShard.ok()) {
            return Status(ErrorCodes::ShardNotFound, "can't find a shard to put new db on");
        }

        log() << "Placing [" << dbName << "] on: " << primaryShard;

        DatabaseType db;
        db.setName(dbName);
        db.setPrimary(primaryShard.getName());
        db.setSharded(false);

        BatchedCommandResponse response;
        status = insert(DatabaseType::ConfigNS, db.toBSON(), &response);
        if (status.isOK()) {
            return status;
        }

        if (status.code() == ErrorCodes::DuplicateKey) {
            return Status(ErrorCodes::NamespaceExists, "database " + dbName + " already exists");
        }

        return Status(status.code(), str::stream() << "database metadata write failed for "
                                                   << dbName << ". Error: " << response.toBSON());
    }

    StatusWith<string> CatalogManagerLegacy::addShard(const string& name,
                                                      const ConnectionString& shardConnectionString,
                                                      const long long maxSize) {

        string shardName;
        ReplicaSetMonitorPtr rsMonitor;
        vector<string> dbNames;

        try {
            ScopedDbConnection newShardConn(shardConnectionString);
            newShardConn->getLastError();

            StatusWith<string> validShard = isValidShard(name,
                                                         shardConnectionString,
                                                         newShardConn);
            if (!validShard.isOK()) {
                newShardConn.done();
                return validShard.getStatus();
            }
            shardName = validShard.getValue();

            StatusWith<vector<string>> shardDBNames = getDBNames(shardConnectionString,
                                                                 newShardConn);
            if (!shardDBNames.isOK()) {
                newShardConn.done();
                return shardDBNames.getStatus();
            }
            dbNames = shardDBNames.getValue();

            if (newShardConn->type() == ConnectionString::SET) {
                rsMonitor = ReplicaSetMonitor::get(shardConnectionString.getSetName());
            }

            newShardConn.done();
        }
        catch (const DBException& e) {
            if (shardConnectionString.type() == ConnectionString::SET) {
                ReplicaSetMonitor::remove(shardConnectionString.getSetName());
            }
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "couldn't connect to new shard "
                                        << e.what());
        }

        // check that none of the existing shard candidate's db's exist elsewhere
        for (vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
            StatusWith<DatabaseType> dbt = getDatabase(*it);
            if (dbt.isOK()) {
                return Status(ErrorCodes::OperationFailed,
                              str::stream() << "can't add shard "
                                            << "'" << shardConnectionString.toString() << "'"
                                            << " because a local database '" << *it
                                            << "' exists in another "
                                            << dbt.getValue().getPrimary());
            }
        }

        // if a name for a shard wasn't provided, pick one.
        if (shardName.empty()) {
            StatusWith<string> result = _getNewShardName();
            if (!result.isOK()) {
                return Status(ErrorCodes::OperationFailed,
                              "error generating new shard name");
            }
            shardName = result.getValue();
        }

        // build the ConfigDB shard document
        BSONObjBuilder b;
        b.append(ShardType::name(), shardName);
        b.append(ShardType::host(),
                 rsMonitor ? rsMonitor->getServerAddress() : shardConnectionString.toString());
        if (maxSize > 0) {
            b.append(ShardType::maxSize(), maxSize);
        }
        BSONObj shardDoc = b.obj();

        if (isShardHost(shardConnectionString)) {
            return Status(ErrorCodes::OperationFailed, "host already used");
        }

        log() << "going to add shard: " << shardDoc;

        Status result = insert(ShardType::ConfigNS, shardDoc, NULL);
        if (!result.isOK()) {
            log() << "error adding shard: " << shardDoc << " err: " << result.reason();
            return result;
        }

        Shard::reloadShardInfo();

        // add all databases of the new shard
        for (vector<string>::const_iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
            DatabaseType dbt;
            dbt.setName(*it);
            dbt.setPrimary(shardName);
            dbt.setSharded(false);
            Status status  = updateDatabase(*it, dbt);
            if (!status.isOK()) {
                log() << "adding shard " << shardConnectionString.toString()
                      << " even though could not add database " << *it;
            }
        }

        // Record in changelog
        BSONObjBuilder shardDetails;
        shardDetails.append("name", shardName);
        shardDetails.append("host", shardConnectionString.toString());

        logChange(NULL, "addShard", "", shardDetails.obj());

        return shardName;
    }

    StatusWith<ShardDrainingStatus> CatalogManagerLegacy::removeShard(OperationContext* txn,
                                                                      const std::string& name) {
        ScopedDbConnection conn(_configServerConnectionString, 30);

        if (conn->count(ShardType::ConfigNS,
                        BSON(ShardType::name() << NE << name
                             << ShardType::draining(true)))) {
            conn.done();
            return Status(ErrorCodes::ConflictingOperationInProgress,
                          "Can't have more than one draining shard at a time");
        }

        if (conn->count(ShardType::ConfigNS,
                        BSON(ShardType::name() << NE << name)) == 0) {
            conn.done();
            return Status(ErrorCodes::IllegalOperation,
                          "Can't remove last shard");
        }

        BSONObj searchDoc = BSON(ShardType::name() << name);

        // Case 1: start draining chunks
        BSONObj drainingDoc =
            BSON(ShardType::name() << name << ShardType::draining(true));
        BSONObj shardDoc = conn->findOne(ShardType::ConfigNS, drainingDoc);
        if (shardDoc.isEmpty()) {
            log() << "going to start draining shard: " << name;
            BSONObj newStatus = BSON("$set" << BSON(ShardType::draining(true)));

            Status status = update(ShardType::ConfigNS, searchDoc, newStatus, false, false, NULL);
            if (!status.isOK()) {
                log() << "error starting removeShard: " << name
                      << "; err: " << status.reason();
                return status;
            }

            BSONObj primaryLocalDoc = BSON(DatabaseType::name("local") <<
                                           DatabaseType::primary(name));
            log() << "primaryLocalDoc: " << primaryLocalDoc;
            if (conn->count(DatabaseType::ConfigNS, primaryLocalDoc)) {
                log() << "This shard is listed as primary of local db. Removing entry.";

                Status status = remove(DatabaseType::ConfigNS,
                                       BSON(DatabaseType::name("local")),
                                       0,
                                       NULL);
                if (!status.isOK()) {
                    log() << "error removing local db: "
                          << status.reason();
                    return status;
                }
            }

            Shard::reloadShardInfo();
            conn.done();

            // Record start in changelog
            logChange(txn, "removeShard.start", "", buildRemoveLogEntry(name, true));
            return ShardDrainingStatus::STARTED;
        }

        // Case 2: all chunks drained
        BSONObj shardIDDoc = BSON(ChunkType::shard(shardDoc[ShardType::name()].str()));
        long long chunkCount = conn->count(ChunkType::ConfigNS, shardIDDoc);
        long long dbCount = conn->count(DatabaseType::ConfigNS,
                                        BSON(DatabaseType::name.ne("local")
                                             << DatabaseType::primary(name)));
        if (chunkCount == 0 && dbCount == 0) {
            log() << "going to remove shard: " << name;
            audit::logRemoveShard(ClientBasic::getCurrent(), name);

            Status status = remove(ShardType::ConfigNS, searchDoc, 0, NULL);
            if (!status.isOK()) {
                log() << "Error concluding removeShard operation on: " << name
                      << "; err: " << status.reason();
                return status;
            }

            Shard::removeShard(name);
            shardConnectionPool.removeHost(name);
            ReplicaSetMonitor::remove(name, true);

            Shard::reloadShardInfo();
            conn.done();

            // Record finish in changelog
            logChange(txn, "removeShard", "", buildRemoveLogEntry(name, false));
            return ShardDrainingStatus::COMPLETED;
        }

        // case 3: draining ongoing
        return ShardDrainingStatus::ONGOING;
    }

    Status CatalogManagerLegacy::updateDatabase(const std::string& dbName, const DatabaseType& db) {
        fassert(28616, db.validate());

        BatchedCommandResponse response;
        Status status = update(DatabaseType::ConfigNS,
                               BSON(DatabaseType::name(dbName)),
                               db.toBSON(),
                               true,     // upsert
                               false,    // multi
                               &response);
        if (!status.isOK()) {
            return Status(status.code(),
                          str::stream() << "database metadata write failed: "
                                        << response.toBSON() << "; status: " << status.toString());
        }

        return Status::OK();
    }

    StatusWith<DatabaseType> CatalogManagerLegacy::getDatabase(const std::string& dbName) {
        invariant(nsIsDbOnly(dbName));

        // The two databases that are hosted on the config server are config and admin
        if (dbName == "config" || dbName == "admin") {
            DatabaseType dbt;
            dbt.setName(dbName);
            dbt.setSharded(false);
            dbt.setPrimary("config");

            return dbt;
        }

        ScopedDbConnection conn(_configServerConnectionString, 30.0);

        BSONObj dbObj = conn->findOne(DatabaseType::ConfigNS, BSON(DatabaseType::name(dbName)));
        if (dbObj.isEmpty()) {
            conn.done();
            return Status(ErrorCodes::DatabaseNotFound,
                          stream() <<  "database " << dbName << " not found");
        }

        conn.done();
        return DatabaseType::fromBSON(dbObj);
    }

    Status CatalogManagerLegacy::updateCollection(const std::string& collNs,
                                                  const CollectionType& coll) {
        fassert(28634, coll.validate());

        BatchedCommandResponse response;
        Status status = update(CollectionType::ConfigNS,
                               BSON(CollectionType::fullNs(collNs)),
                               coll.toBSON(),
                               true,    // upsert
                               false,   // multi
                               NULL);
        if (!status.isOK()) {
            return Status(status.code(),
                          str::stream() << "collection metadata write failed: "
                                        << response.toBSON() << "; status: " << status.toString());
        }

        return Status::OK();
    }

    StatusWith<CollectionType> CatalogManagerLegacy::getCollection(const std::string& collNs) {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);

        BSONObj collObj = conn->findOne(CollectionType::ConfigNS,
                                        BSON(CollectionType::fullNs(collNs)));
        if (collObj.isEmpty()) {
            conn.done();
            return Status(ErrorCodes::NamespaceNotFound,
                          stream() << "collection " << collNs << " not found");
        }

        conn.done();
        return CollectionType::fromBSON(collObj);
    }

    Status CatalogManagerLegacy::getCollections(const std::string* dbName,
                                                std::vector<CollectionType>* collections) {
        collections->clear();

        BSONObjBuilder b;
        if (dbName) {
            invariant(!dbName->empty());
            b.appendRegex(CollectionType::fullNs(),
                          (string)"^" + pcrecpp::RE::QuoteMeta(*dbName) + "\\.");
        }

        ScopedDbConnection conn(_configServerConnectionString, 30.0);

        std::unique_ptr<DBClientCursor> cursor(_safeCursor(conn->query(CollectionType::ConfigNS,
                                                                       b.obj())));

        while (cursor->more()) {
            const BSONObj collObj = cursor->next();

            auto status = CollectionType::fromBSON(collObj);
            if (!status.isOK()) {
                conn.done();
                return status.getStatus();
            }

            collections->push_back(status.getValue());
        }

        conn.done();
        return Status::OK();
    }

    Status CatalogManagerLegacy::dropCollection(const std::string& collectionNs) {
        logChange(NULL, "dropCollection.start", collectionNs, BSONObj());

        // Lock the collection globally so that split/migrate cannot run
        auto scopedDistLock = getDistLockManager()->lock(collectionNs, "drop");
        if (!scopedDistLock.isOK()) {
            return scopedDistLock.getStatus();
        }

        LOG(1) << "dropCollection " << collectionNs << " started";

        // This cleans up the collection on all shards
        vector<ShardType> allShards;
        Status status = getAllShards(&allShards);
        if (!status.isOK()) {
            return status;
        }

        LOG(1) << "dropCollection " << collectionNs << " locked";

        map<string, BSONObj> errors;

        // Delete data from all mongods
        for (vector<ShardType>::const_iterator i = allShards.begin(); i != allShards.end(); i++) {
            Shard shard = Shard::make(i->getHost());
            ScopedDbConnection conn(shard.getConnString());

            BSONObj info;
            if (!conn->dropCollection(collectionNs, &info)) {
                // Ignore the database not found errors
                if (info["code"].isNumber() &&
                        (info["code"].Int() == ErrorCodes::NamespaceNotFound)) {
                    conn.done();
                    continue;
                }
                errors[shard.getConnString()] = info;
            }

            conn.done();
        }

        if (!errors.empty()) {
            StringBuilder sb;
            sb << "Dropping collection failed on the following hosts: ";

            for (map<string, BSONObj>::const_iterator it = errors.begin();
                 it != errors.end();
                 ++it) {

                if (it != errors.begin()) {
                    sb << ", ";
                }

                sb << it->first << ": " << it->second;
            }

            return Status(ErrorCodes::OperationFailed, sb.str());
        }

        LOG(1) << "dropCollection " << collectionNs << " shard data deleted";

        // remove chunk data
        Status result = remove(ChunkType::ConfigNS,
                               BSON(ChunkType::ns(collectionNs)),
                               0,
                               NULL);
        if (!result.isOK()) {
            return result;
        }

        LOG(1) << "dropCollection " << collectionNs << " chunk data deleted";

        for (vector<ShardType>::const_iterator i = allShards.begin(); i != allShards.end(); i++) {
            Shard shard = Shard::make(i->getHost());
            ScopedDbConnection conn(shard.getConnString());

            BSONObj res;

            // this is horrible
            // we need a special command for dropping on the d side
            // this hack works for the moment

            if (!setShardVersion(conn.conn(),
                                 collectionNs,
                                 _configServerConnectionString.toString(),
                                 ChunkVersion(0, 0, OID()),
                                 NULL,
                                 true,
                                 res)) {

                return Status(static_cast<ErrorCodes::Error>(8071),
                              str::stream() << "cleaning up after drop failed: " << res);
            }

            conn->simpleCommand("admin", 0, "unsetSharding");
            conn.done();
        }

        LOG(1) << "dropCollection " << collectionNs << " completed";

        logChange(NULL, "dropCollection", collectionNs, BSONObj());

        return Status::OK();
    }

    void CatalogManagerLegacy::logAction(const ActionLogType& actionLog) {
        // Create the action log collection and ensure that it is capped. Wrap in try/catch,
        // because creating an existing collection throws.
        if (actionLogCollectionCreated.load() == 0) {
            try {
                ScopedDbConnection conn(_configServerConnectionString, 30.0);
                conn->createCollection(ActionLogType::ConfigNS, 1024 * 1024 * 2, true);
                conn.done();

                actionLogCollectionCreated.store(1);
            }
            catch (const DBException& e) {
                // It's ok to ignore this exception
                LOG(1) << "couldn't create actionlog collection: " << e;
            }
        }

        Status result = insert(ActionLogType::ConfigNS, actionLog.toBSON(), NULL);
        if (!result.isOK()) {
            log() << "error encountered while logging action: " << result;
        }
    }

    void CatalogManagerLegacy::logChange(OperationContext* opCtx,
                                         const string& what,
                                         const string& ns,
                                         const BSONObj& detail) {

        // Create the change log collection and ensure that it is capped. Wrap in try/catch,
        // because creating an existing collection throws.
        if (changeLogCollectionCreated.load() == 0) {
            try {
                ScopedDbConnection conn(_configServerConnectionString, 30.0);
                conn->createCollection(ChangelogType::ConfigNS, 1024 * 1024 * 10, true);
                conn.done();

                changeLogCollectionCreated.store(1);
            }
            catch (const UserException& e) {
                // It's ok to ignore this exception
                LOG(1) << "couldn't create changelog collection: " << e;
            }
        }

        // Store this entry's ID so we can use on the exception code path too
        StringBuilder changeIdBuilder;
        changeIdBuilder << getHostNameCached() << "-" << terseCurrentTime()
                        << "-" << OID::gen();

        const string changeID = changeIdBuilder.str();

        Client* client;
        if (opCtx) {
            client = opCtx->getClient();
        }
        else if (haveClient()) {
            client = &cc();
        }
        else {
            client = nullptr;
        }

        // Send a copy of the message to the local log in case it doesn't manage to reach
        // config.changelog
        BSONObj msg = BSON(ChangelogType::changeID(changeID) <<
                           ChangelogType::server(getHostNameCached()) <<
                           ChangelogType::clientAddr((client ?
                                                        client->clientAddress(true) : "")) <<
                           ChangelogType::time(jsTime()) <<
                           ChangelogType::what(what) <<
                           ChangelogType::ns(ns) <<
                           ChangelogType::details(detail));

        log() << "about to log metadata event: " << msg;

        Status result = insert(ChangelogType::ConfigNS, msg, NULL);
        if (!result.isOK()) {
            warning() << "Error encountered while logging config change with ID "
                      << changeID << ": " << result;
        }
    }

    StatusWith<SettingsType> CatalogManagerLegacy::getGlobalSettings(const string& key) {
        try {
            ScopedDbConnection conn(_configServerConnectionString, 30);
            BSONObj settingsDoc = conn->findOne(SettingsType::ConfigNS,
                                                BSON(SettingsType::key(key)));
            StatusWith<SettingsType> settingsResult = SettingsType::fromBSON(settingsDoc);
            conn.done();

            if (!settingsResult.isOK()) {
                return settingsResult.getStatus();
            }
            const SettingsType& settings = settingsResult.getValue();
            Status validationStatus = settings.validate();
            if (!validationStatus.isOK()) {
                return validationStatus;
            }

            return settingsResult;
        }
        catch (const DBException& ex) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "unable to successfully obtain "
                                        << "config.settings document: " << causedBy(ex));
        }
    }

    Status CatalogManagerLegacy::getDatabasesForShard(const string& shardName,
                                                      vector<string>* dbs) {
        dbs->clear();

        try {
            ScopedDbConnection conn(_configServerConnectionString, 30.0);
            std::unique_ptr<DBClientCursor> cursor(_safeCursor(
                                conn->query(DatabaseType::ConfigNS,
                                            Query(BSON(DatabaseType::primary(shardName))))));
            if (!cursor.get()) {
                conn.done();
                return Status(ErrorCodes::HostUnreachable, "unable to open chunk cursor");
            }

            while (cursor->more()) {
                BSONObj shard = cursor->nextSafe();
                dbs->push_back(shard[DatabaseType::name()].str());
            }

            conn.done();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

        return Status::OK();
    }

    Status CatalogManagerLegacy::getChunks(const Query& query,
                                           int nToReturn,
                                           vector<ChunkType>* chunks) {
        chunks->clear();

        try {
            ScopedDbConnection conn(_configServerConnectionString, 30.0);
            std::unique_ptr<DBClientCursor> cursor(_safeCursor(conn->query(ChunkType::ConfigNS,
                                                                           query,
                                                                           nToReturn)));
            if (!cursor.get()) {
                conn.done();
                return Status(ErrorCodes::HostUnreachable, "unable to open chunk cursor");
            }

            while (cursor->more()) {
                BSONObj chunkObj = cursor->nextSafe();

                StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(chunkObj);
                if (!chunkRes.isOK()) {
                    conn.done();
                    return Status(ErrorCodes::FailedToParse,
                                  str::stream() << "Failed to parse chunk BSONObj: "
                                                << chunkRes.getStatus().reason());
                }

                chunks->push_back(chunkRes.getValue());
            }

            conn.done();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

        return Status::OK();
    }

    Status CatalogManagerLegacy::getTagsForCollection(const std::string& collectionNs,
                                                      std::vector<TagsType>* tags) {
        tags->clear();

        try {
            ScopedDbConnection conn(_configServerConnectionString, 30);
            std::unique_ptr<DBClientCursor> cursor(_safeCursor(
                                conn->query(TagsType::ConfigNS,
                                            Query(BSON(TagsType::ns(collectionNs)))
                                                .sort(TagsType::min()))));
            if (!cursor.get()) {
                conn.done();
                return Status(ErrorCodes::HostUnreachable, "unable to open tags cursor");
            }

            while (cursor->more()) {
                BSONObj tagObj = cursor->nextSafe();

                StatusWith<TagsType> tagRes = TagsType::fromBSON(tagObj);
                if (!tagRes.isOK()) {
                    conn.done();
                    return Status(ErrorCodes::FailedToParse,
                                  str::stream() << "Failed to parse tag BSONObj: "
                                                << tagRes.getStatus().reason());
                }

                tags->push_back(tagRes.getValue());
            }

            conn.done();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

        return Status::OK();
    }

    StatusWith<string> CatalogManagerLegacy::getTagForChunk(const std::string& collectionNs,
                                                            const ChunkType& chunk) {
        BSONObj tagDoc;

        try {
            ScopedDbConnection conn(_configServerConnectionString, 30);

            Query query(BSON(TagsType::ns(collectionNs) <<
                             TagsType::min() << BSON("$lte" << chunk.getMin()) <<
                             TagsType::max() << BSON("$gte" << chunk.getMax())));

            tagDoc = conn->findOne(TagsType::ConfigNS, query);
            conn.done();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (tagDoc.isEmpty()) {
            return std::string("");
        }

        auto status = TagsType::fromBSON(tagDoc);
        if (status.isOK()) {
            return status.getValue().getTag();
        }

        return status.getStatus();
    }

    Status CatalogManagerLegacy::getAllShards(vector<ShardType>* shards) {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);
        std::unique_ptr<DBClientCursor> cursor(_safeCursor(conn->query(ShardType::ConfigNS,
                                                                       BSONObj())));
        while (cursor->more()) {
            BSONObj shardObj = cursor->nextSafe();

            StatusWith<ShardType> shardRes = ShardType::fromBSON(shardObj);
            if (!shardRes.isOK()) {
                conn.done();
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Failed to parse chunk BSONObj: "
                                            << shardRes.getStatus().reason());
            }

            ShardType shard = shardRes.getValue();
            shards->push_back(shard);
        }
        conn.done();

        return Status::OK();
    }

    bool CatalogManagerLegacy::isShardHost(const ConnectionString& connectionString) {
        return _getShardCount(BSON(ShardType::host(connectionString.toString())));
    }

    bool CatalogManagerLegacy::doShardsExist() {
        return _getShardCount() > 0;
    }

    bool CatalogManagerLegacy::runUserManagementWriteCommand(const string& commandName,
                                                             const string& dbname,
                                                             const BSONObj& cmdObj,
                                                             BSONObjBuilder* result) {
        DBClientMultiCommand dispatcher;
        RawBSONSerializable requestCmdSerial(cmdObj);
        for (const ConnectionString& configServer : _configServers) {
            dispatcher.addCommand(configServer, dbname, requestCmdSerial);
        }

        auto scopedDistLock = getDistLockManager()->lock("authorizationData",
                                                         commandName,
                                                         Seconds{5});
        if (!scopedDistLock.isOK()) {
            return Command::appendCommandStatus(*result, scopedDistLock.getStatus());
        }

        dispatcher.sendAll();

        BSONObj responseObj;

        Status prevStatus{Status::OK()};
        Status currStatus{Status::OK()};

        BSONObjBuilder responses;
        unsigned failedCount = 0;
        bool sameError = true;
        while (dispatcher.numPending() > 0) {
            ConnectionString host;
            RawBSONSerializable responseCmdSerial;

            Status dispatchStatus = dispatcher.recvAny(&host,
                                                       &responseCmdSerial);

            if (!dispatchStatus.isOK()) {
                return Command::appendCommandStatus(*result, dispatchStatus);
            }

            responseObj = responseCmdSerial.toBSON();
            responses.append(host.toString(), responseObj);

            currStatus = Command::getStatusFromCommandResult(responseObj);
            if (!currStatus.isOK()) {
                // same error <=> adjacent error statuses are the same
                if (failedCount > 0 && prevStatus != currStatus) {
                    sameError = false;
                }
                failedCount++;
                prevStatus = currStatus;
            }
        }

        if (failedCount == 0) {
            result->appendElements(responseObj);
            return true;
        }

        // if the command succeeds on at least one config server and fails on at least one,
        // manual intervention is required
        if (failedCount < _configServers.size()) {
            Status status(ErrorCodes::ManualInterventionRequired,
                          str::stream() << "Config write was not consistent - "
                                        << "user management command failed on at least one "
                                        << "config server but passed on at least one other. "
                                        << "Manual intervention may be required. "
                                        << "Config responses: " << responses.obj().toString());
            return Command::appendCommandStatus(*result, status);
        }

        if (sameError) {
            result->appendElements(responseObj);
            return false;
        }

        Status status(ErrorCodes::ManualInterventionRequired,
                      str::stream() << "Config write was not consistent - "
                                    << "user management command produced inconsistent results. "
                                    << "Manual intervention may be required. "
                                    << "Config responses: " << responses.obj().toString());
        return Command::appendCommandStatus(*result, status);
    }

    bool CatalogManagerLegacy::runUserManagementReadCommand(const string& dbname,
                                                            const BSONObj& cmdObj,
                                                            BSONObjBuilder* result) {
        try {
            // let SyncClusterConnection handle connecting to the first config server
            // that is reachable and returns data
            ScopedDbConnection conn(_configServerConnectionString, 30);

            BSONObj cmdResult;
            const bool ok = conn->runCommand(dbname, cmdObj, cmdResult);
            result->appendElements(cmdResult);
            conn.done();
            return ok;
        }
        catch (const DBException& ex) {
            return Command::appendCommandStatus(*result, ex.toStatus());
        }
    }

    Status CatalogManagerLegacy::applyChunkOpsDeprecated(const BSONArray& updateOps,
                                                         const BSONArray& preCondition) {
        BSONObj cmd = BSON("applyOps" << updateOps <<
                           "preCondition" << preCondition);
        bool ok;
        BSONObj cmdResult;
        try {
            ScopedDbConnection conn(_configServerConnectionString, 30);
            ok = conn->runCommand("config", cmd, cmdResult);
            conn.done();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (!ok) {
            string errMsg(str::stream() << "Unable to save chunk ops. Command: "
                                        << cmd << ". Result: " << cmdResult);

            return Status(ErrorCodes::OperationFailed, errMsg);
        }

        return Status::OK();
    }

    void CatalogManagerLegacy::writeConfigServerDirect(const BatchedCommandRequest& request,
                                                       BatchedCommandResponse* response) {

        // check if config servers are consistent
        if (!_isConsistentFromLastCheck()) {
            toBatchError(
                Status(ErrorCodes::IncompatibleShardingMetadata,
                       "Data inconsistency detected amongst config servers"),
                response);
            return;
        }

        // We only support batch sizes of one for config writes
        if (request.sizeWriteOps() != 1) {
            toBatchError(
                Status(ErrorCodes::InvalidOptions,
                       str::stream() << "Writes to config servers must have batch size of 1, "
                                     << "found " << request.sizeWriteOps()),
                response);

            return;
        }

        // We only support {w: 0}, {w: 1}, and {w: 'majority'} write concern for config writes
        if (request.isWriteConcernSet() && !validConfigWC(request.getWriteConcern())) {
            toBatchError(
                Status(ErrorCodes::InvalidOptions,
                       str::stream() << "Invalid write concern for write to "
                                     << "config servers: " << request.getWriteConcern()),
                response);

            return;
        }

        DBClientMultiCommand dispatcher;
        if (_configServers.size() > 1) {
            // We can't support no-_id upserts to multiple config servers - the _ids will differ
            if (BatchedCommandRequest::containsNoIDUpsert(request)) {
                toBatchError(
                    Status(ErrorCodes::InvalidOptions,
                           str::stream() << "upserts to multiple config servers must include _id"),
                    response);
                return;
            }
        }

        ConfigCoordinator exec(&dispatcher, _configServers);
        exec.executeBatch(request, response);
    }

    Status CatalogManagerLegacy::_checkDbDoesNotExist(const std::string& dbName) const {
        ScopedDbConnection conn(_configServerConnectionString, 30);

        BSONObjBuilder b;
        b.appendRegex(DatabaseType::name(),
                      (string)"^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

        BSONObj dbObj = conn->findOne(DatabaseType::ConfigNS, b.obj());
        conn.done();

        // If our name is exactly the same as the name we want, try loading
        // the database again.
        if (!dbObj.isEmpty() && dbObj[DatabaseType::name()].String() == dbName) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "database " << dbName << " already exists");
        }

        if (!dbObj.isEmpty()) {
            return Status(ErrorCodes::DatabaseDifferCase,
                          str::stream() << "can't have 2 databases that just differ on case "
                                        << " have: " << dbObj[DatabaseType::name()].String()
                                        << " want to add: " << dbName);
        }

        return Status::OK();
    }

    StatusWith<string> CatalogManagerLegacy::_getNewShardName() const {
        BSONObj o;
        {
            ScopedDbConnection conn(_configServerConnectionString, 30);
            o = conn->findOne(ShardType::ConfigNS,
                              Query(fromjson("{" + ShardType::name() + ": /^shard/}"))
                                  .sort(BSON(ShardType::name() << -1 )));
            conn.done();
        }

        int count = 0;
        if (!o.isEmpty()) {
            string last = o[ShardType::name()].String();
            std::istringstream is(last.substr(5));
            is >> count;
            count++;
        }

        // TODO fix so that we can have more than 10000 automatically generated shard names
        if (count < 9999) {
            std::stringstream ss;
            ss << "shard" << std::setfill('0') << std::setw(4) << count;
            return ss.str();
        }

        return Status(ErrorCodes::OperationFailed,
                      "unable to generate new shard name");
    }

    size_t CatalogManagerLegacy::_getShardCount(const BSONObj& query) const {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);
        long long shardCount = conn->count(ShardType::ConfigNS, query);
        conn.done();

        return shardCount;
    }

    DistLockManager* CatalogManagerLegacy::getDistLockManager() {
        invariant(_distLockManager);
        return _distLockManager.get();
    }

    bool CatalogManagerLegacy::_checkConfigServersConsistent(const unsigned tries) const {
        if (tries <= 0)
            return false;

        unsigned firstGood = 0;
        int up = 0;
        vector<BSONObj> res;

        // The last error we saw on a config server
        string errMsg;

        for (unsigned i = 0; i < _configServers.size(); i++) {
            BSONObj result;
            std::unique_ptr<ScopedDbConnection> conn;

            try {
                conn.reset(new ScopedDbConnection(_configServers[i], 30.0));

                if (!conn->get()->runCommand("config",
                                             BSON("dbhash" << 1 <<
                                                  "collections" << BSON_ARRAY("chunks" <<
                                                                              "databases" <<
                                                                              "collections" <<
                                                                              "shards" <<
                                                                              "version")),
                                              result)) {

                    errMsg = result["errmsg"].eoo() ? "" : result["errmsg"].String();
                    if (!result["assertion"].eoo()) errMsg = result["assertion"].String();

                    warning() << "couldn't check dbhash on config server " << _configServers[i]
                              << causedBy(result.toString());

                    result = BSONObj();
                }
                else {
                    result = result.getOwned();
                    if (up == 0)
                        firstGood = i;
                    up++;
                }
                conn->done();
            }
            catch (const DBException& e) {
                if (conn) {
                    conn->kill();
                }

                // We need to catch DBExceptions b/c sometimes we throw them
                // instead of socket exceptions when findN fails

                errMsg = e.toString();
                warning() << " couldn't check dbhash on config server "
                          << _configServers[i] << causedBy(e);
            }
            res.push_back(result);
        }

        if (_configServers.size() == 1)
            return true;

        if (up == 0) {
            // Use a ptr to error so if empty we won't add causedby
            error() << "no config servers successfully contacted" << causedBy(&errMsg);
            return false;
        }
        else if (up == 1) {
            warning() << "only 1 config server reachable, continuing";
            return true;
        }

        BSONObj base = res[firstGood];
        for (unsigned i = firstGood+1; i < res.size(); i++) {
            if (res[i].isEmpty())
                continue;

            string chunksHash1 = base.getFieldDotted("collections.chunks");
            string chunksHash2 = res[i].getFieldDotted("collections.chunks");

            string databaseHash1 = base.getFieldDotted("collections.databases");
            string databaseHash2 = res[i].getFieldDotted("collections.databases");

            string collectionsHash1 = base.getFieldDotted("collections.collections");
            string collectionsHash2 = res[i].getFieldDotted("collections.collections");

            string shardHash1 = base.getFieldDotted("collections.shards");
            string shardHash2 = res[i].getFieldDotted("collections.shards");

            string versionHash1 = base.getFieldDotted("collections.version") ;
            string versionHash2 = res[i].getFieldDotted("collections.version");

            if (chunksHash1 == chunksHash2 &&
                databaseHash1 == databaseHash2 &&
                collectionsHash1 == collectionsHash2 &&
                shardHash1 == shardHash2 &&
                versionHash1 == versionHash2) {
                continue;
            }

            warning() << "config servers " << _configServers[firstGood].toString()
                      << " and " << _configServers[i].toString() << " differ";
            if (tries <= 1) {
                error() << ": " << base["collections"].Obj()
                        << " vs " << res[i]["collections"].Obj();
                return false;
            }

            return _checkConfigServersConsistent(tries - 1);
        }

        return true;
    }

    void CatalogManagerLegacy::_consistencyChecker() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_inShutdown) {
            lk.unlock();
            const bool isConsistent = _checkConfigServersConsistent();

            lk.lock();
            _consistentFromLastCheck = isConsistent;
            if (_inShutdown) break;
            _consistencyCheckerCV.wait_for(lk, Seconds(60));
        }
        LOG(1) << "Consistency checker thread shutting down";
    }

    bool CatalogManagerLegacy::_isConsistentFromLastCheck() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        return _consistentFromLastCheck;
    }

} // namespace mongo
