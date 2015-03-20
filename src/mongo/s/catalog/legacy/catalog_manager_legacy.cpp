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

#include <pcrecpp.h>
#include <set>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/legacy/config_coordinator.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/client/dbclient_multi_command.h"
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

    // Whether the logChange call should attempt to create the changelog collection
    AtomicInt32 changeLogCollectionCreated(0);

} // namespace


    Status CatalogManagerLegacy::init(const vector<string>& configHosts) {
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

        _configServerConnectionString = ConnectionString(fullString, ConnectionString::SYNC);

        if (_configServerConnectionString.type() == ConnectionString::MASTER) {
            _configServers.push_back(_configServerConnectionString);
        }
        else if (_configServerConnectionString.type() == ConnectionString::SYNC) {
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

        try {
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
        catch (DBException& e) {
            e.addContext("error creating initial database config information");
            warning() << e.what();

            return e.toStatus();
        }
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
        ScopedDbConnection conn(_configServerConnectionString, 30.0);

        BSONObj dbObj = conn->findOne(DatabaseType::ConfigNS, BSON(DatabaseType::name(dbName)));
        if (dbObj.isEmpty()) {
            conn.done();
            return Status(ErrorCodes::DatabaseNotFound,
                          stream() <<  "database " << dbName << " not found.");
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

} // namespace mongo
