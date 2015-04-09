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
#include <pcrecpp.h>
#include <set>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/legacy/config_coordinator.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/dbclient_multi_command.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/shard.h"
#include "mongo/s/type_database.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

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

} // namespace


    Status CatalogManagerLegacy::init(const vector<string>& configHosts) {
        // Initialization should not happen more than once
        invariant(!_configServerConnectionString.isValid());
        invariant(_configServers.empty());

        if (configHosts.empty()) {
            return Status(ErrorCodes::InvalidOptions, "No config server hosts specified");
        }
        
        // Extract the hosts in HOST:PORT format
        set<HostAndPort> configHostsAndPorts;
        set<string> configHostsOnly;
        for (size_t i = 0; i < configHosts.size(); i++) {
            // Parse the config host string
            StatusWith<HostAndPort> status = HostAndPort::parse(configHosts[i]);
            if (!status.isOK()) {
                return status.getStatus();
            }

            // Append the default port, if not specified
            HostAndPort configHost = status.getValue();
            if (!configHost.hasPort()) {
                configHost = HostAndPort(configHost.host(), ServerGlobalParams::ConfigServerPort);
            }

            // Make sure there are no duplicates
            if (!configHostsAndPorts.insert(configHost).second) {
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

        string fullString;
        joinStringDelim(configHosts, &fullString, ',');

        LOG(1) << " config string : " << fullString;

        // Now that the config hosts are verified, initialize the catalog manager. The code below
        // should never fail.

        _configServerConnectionString = ConnectionString(fullString, ConnectionString::SYNC);

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

        return Status::OK();
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

    Status CatalogManagerLegacy::createDatabase(const std::string& dbName, const Shard* shard) {
        invariant(nsIsDbOnly(dbName));

        // The admin and config databases should never be explicitly created. They "just exist",
        // i.e. getDatabase will always return an entry for them.
        invariant(dbName != "admin");
        invariant(dbName != "config");

        // Check for case sensitivity violations
        Status status = _checkDbDoesNotExist(dbName);
        if (!status.isOK()) {
            return status;
        }

        // Database does not exist, pick a shard and create a new entry
        const Shard primaryShard = (shard ? *shard : Shard::pick());
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

        {
            ScopedDbConnection conn(_configServerConnectionString, 30);

            // check whether the set of hosts (or single host) is not an already a known shard
            BSONObj old = conn->findOne(ShardType::ConfigNS,
                                        BSON(ShardType::host(shardConnectionString.toString())));

            if (!old.isEmpty()) {
                conn.done();
                return Status(ErrorCodes::OperationFailed, "host already used");
            }
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
                                          << response.toBSON());
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

        return DatabaseType::fromBSON(dbObj);
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

        Client* const client = (opCtx ? opCtx->getClient() : currentClient.get());

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

    void CatalogManagerLegacy::getDatabasesForShard(const string& shardName,
                                                    vector<string>* dbs) {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);
        BSONObj prim = BSON(DatabaseType::primary(shardName));
        boost::scoped_ptr<DBClientCursor> cursor(conn->query(DatabaseType::ConfigNS, prim));

        while (cursor->more()) {
            BSONObj shard = cursor->nextSafe();
            dbs->push_back(shard[DatabaseType::name()].str());
        }

        conn.done();
    }

    Status CatalogManagerLegacy::getChunksForShard(const string& shardName,
                                                   vector<ChunkType>* chunks) {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);
        boost::scoped_ptr<DBClientCursor> cursor(conn->query(ChunkType::ConfigNS,
                                                             BSON(ChunkType::shard(shardName))));
        while (cursor->more()) {
            BSONObj chunkObj = cursor->nextSafe();

            StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(chunkObj);
            if (!chunkRes.isOK()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Failed to parse chunk BSONObj: "
                                            << chunkRes.getStatus().reason());
            }
            ChunkType chunk = chunkRes.getValue();
            chunks->push_back(chunk);
        }
        conn.done();

        return Status::OK();
    }

    Status CatalogManagerLegacy::getAllShards(vector<ShardType>* shards) {
        ScopedDbConnection conn(_configServerConnectionString, 30.0);
        boost::scoped_ptr<DBClientCursor> cursor(conn->query(ShardType::ConfigNS, BSONObj()));
        while (cursor->more()) {
            BSONObj shardObj = cursor->nextSafe();

            StatusWith<ShardType> shardRes = ShardType::fromBSON(shardObj);
            if (!shardRes.isOK()) {
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

    void CatalogManagerLegacy::writeConfigServerDirect(const BatchedCommandRequest& request,
                                                       BatchedCommandResponse* response) {

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
            return Status(static_cast<ErrorCodes::Error>(DatabaseDifferCaseCode),
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

} // namespace mongo
