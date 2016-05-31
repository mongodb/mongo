/**
 *    Copyright (C) 2010-2015 MongoDB Inc.
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

#include "mongo/s/client/version_manager.h"

#include "mongo/client/dbclient_rs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::map;
using std::string;

namespace {

/**
 * Tracking information, per-connection, of the latest chunk manager iteration or sequence
 * number that was used to send a shard version over this connection.
 * When the chunk manager is replaced, implying new versions were loaded, the chunk manager
 * sequence number is iterated by 1 and connections need to re-send shard versions.
 */
class ConnectionShardStatus {
public:
    bool hasAnySequenceSet(DBClientBase* conn) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        SequenceMap::const_iterator seenConnIt = _map.find(conn->getConnectionId());
        return seenConnIt != _map.end() && seenConnIt->second.size() > 0;
    }

    bool getSequence(DBClientBase* conn, const string& ns, unsigned long long* sequence) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        SequenceMap::const_iterator seenConnIt = _map.find(conn->getConnectionId());
        if (seenConnIt == _map.end())
            return false;

        map<string, unsigned long long>::const_iterator seenNSIt = seenConnIt->second.find(ns);
        if (seenNSIt == seenConnIt->second.end())
            return false;

        *sequence = seenNSIt->second;
        return true;
    }

    void setSequence(DBClientBase* conn, const string& ns, const unsigned long long& s) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _map[conn->getConnectionId()][ns] = s;
    }

    void reset(DBClientBase* conn) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _map.erase(conn->getConnectionId());
    }

private:
    // protects _map
    stdx::mutex _mutex;

    // a map from a connection into ChunkManager's sequence number for each namespace
    typedef map<unsigned long long, map<string, unsigned long long>> SequenceMap;
    SequenceMap _map;

} connectionShardStatus;

/**
 * Sends the setShardVersion command on the specified connection.
 */
bool setShardVersion(OperationContext* txn,
                     DBClientBase* conn,
                     const string& ns,
                     const ConnectionString& configServer,
                     ChunkVersion version,
                     ChunkManager* manager,
                     bool authoritative,
                     BSONObj& result) {
    ShardId shardId;
    ConnectionString shardCS;
    {
        const auto shard = grid.shardRegistry()->getShardForHostNoReload(
            uassertStatusOK(HostAndPort::parse(conn->getServerAddress())));
        uassert(ErrorCodes::ShardNotFound,
                str::stream() << conn->getServerAddress() << " is not recognized as a shard",
                shard);

        shardId = shard->getId();
        shardCS = shard->getConnString();
    }

    BSONObj cmd;

    if (ns.empty()) {
        SetShardVersionRequest ssv =
            SetShardVersionRequest::makeForInit(configServer, shardId, shardCS);
        cmd = ssv.toBSON();
    } else {
        SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioning(
            configServer, shardId, shardCS, NamespaceString(ns), version, authoritative);
        cmd = ssv.toBSON();
    }

    LOG(1) << "    setShardVersion  " << shardId << " " << conn->getServerAddress() << "  " << ns
           << "  " << cmd
           << (manager ? string(str::stream() << " " << manager->getSequenceNumber()) : "");

    return conn->runCommand("admin", cmd, result, 0);
}

/**
 * Checks whether the specified connection supports versioning.
 */
DBClientBase* getVersionable(DBClientBase* conn) {
    switch (conn->type()) {
        case ConnectionString::LOCAL:
        case ConnectionString::INVALID:
        case ConnectionString::CUSTOM:
            MONGO_UNREACHABLE;

        case ConnectionString::MASTER:
            return conn;
        case ConnectionString::SET:
            DBClientReplicaSet* set = (DBClientReplicaSet*)conn;
            return &(set->masterConn());
    }

    MONGO_UNREACHABLE;
}

/**
 * Special internal logic to run reduced version handshake for empty namespace operations to
 * shards.
 *
 * Eventually this should go completely away, but for now many commands rely on unversioned but
 * mongos-specific behavior on mongod (auditing and replication information in commands)
 */
bool initShardVersionEmptyNS(OperationContext* txn, DBClientBase* conn_in) {
    try {
        // May throw if replica set primary is down
        DBClientBase* const conn = getVersionable(conn_in);
        dassert(conn);  // errors thrown above

        // Check to see if we've already initialized this connection. This avoids sending
        // setShardVersion multiple times.
        if (connectionShardStatus.hasAnySequenceSet(conn)) {
            return false;
        }

        BSONObj result;
        const bool ok = setShardVersion(txn,
                                        conn,
                                        "",
                                        grid.shardRegistry()->getConfigServerConnectionString(),
                                        ChunkVersion(),
                                        NULL,
                                        true,
                                        result);

        LOG(3) << "initial sharding result : " << result;

        connectionShardStatus.setSequence(conn, "", 0);
        return ok;
    } catch (const DBException&) {
        // NOTE: Replica sets may fail to initShardVersion because future calls relying on
        // correct versioning must later call checkShardVersion on the primary.
        // Secondary queries and commands may not call checkShardVersion, but secondary ops
        // aren't versioned at all.
        if (conn_in->type() != ConnectionString::SET) {
            throw;
        }

        // NOTE: Only old-style cluster operations will talk via DBClientReplicaSets - using
        // checkShardVersion is required (which includes initShardVersion information) if these
        // connections are used.

        OCCASIONALLY {
            warning() << "failed to initialize new replica set connection version, "
                      << "will initialize on first use";
        }

        return false;
    }
}

/**
 * Updates the remote cached version on the remote shard host (primary, in the case of replica
 * sets) if needed with a fully-qualified shard version for the given namespace:
 *   config server(s) + shard name + shard version
 *
 * If no remote cached version has ever been set, an initial shard version is sent.
 *
 * If the namespace is empty and no version has ever been sent, the config server + shard name
 * is sent to the remote shard host to initialize the connection as coming from mongos.
 * NOTE: This initialization is *best-effort only*.  Operations which wish to correctly version
 * must send the namespace.
 *
 * Config servers are special and are not (unless otherwise a shard) kept up to date with this
 * protocol.  This is safe so long as config servers only contain unversioned collections.
 *
 * It is an error to call checkShardVersion with an unversionable connection (isVersionableCB).
 *
 * @return true if we contacted the remote host
 */
bool checkShardVersion(OperationContext* txn,
                       DBClientBase* conn_in,
                       const string& ns,
                       ChunkManagerPtr refManager,
                       bool authoritative,
                       int tryNumber) {
    // Empty namespaces are special - we require initialization but not versioning
    if (ns.size() == 0) {
        return initShardVersionEmptyNS(txn, conn_in);
    }

    auto status = grid.catalogCache()->getDatabase(txn, nsToDatabase(ns));
    if (!status.isOK()) {
        return false;
    }

    DBClientBase* const conn = getVersionable(conn_in);
    verify(conn);  // errors thrown above

    shared_ptr<DBConfig> conf = status.getValue();

    if (authoritative) {
        conf->getChunkManagerIfExists(txn, ns, true);
    }

    shared_ptr<Shard> primary;
    shared_ptr<ChunkManager> manager;

    conf->getChunkManagerOrPrimary(txn, ns, manager, primary);

    unsigned long long officialSequenceNumber = 0;

    if (manager) {
        officialSequenceNumber = manager->getSequenceNumber();
    } else if (primary && primary->isConfig()) {
        // Do not send setShardVersion to collections on the config servers - this causes problems
        // when config servers are also shards and get SSV with conflicting names.
        return false;
    }

    const auto shard = grid.shardRegistry()->getShardForHostNoReload(
        uassertStatusOK(HostAndPort::parse(conn->getServerAddress())));
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << conn->getServerAddress() << " is not recognized as a shard",
            shard);

    // Check this manager against the reference manager
    if (manager) {
        if (refManager && !refManager->compatibleWith(*manager, shard->getId())) {
            const ChunkVersion refVersion(refManager->getVersion(shard->getId()));
            const ChunkVersion currentVersion(manager->getVersion(shard->getId()));

            string msg(str::stream() << "manager (" << currentVersion.toString() << " : "
                                     << manager->getSequenceNumber()
                                     << ") "
                                     << "not compatible with reference manager ("
                                     << refVersion.toString()
                                     << " : "
                                     << refManager->getSequenceNumber()
                                     << ") "
                                     << "on shard "
                                     << shard->getId()
                                     << " ("
                                     << shard->getConnString().toString()
                                     << ")");

            throw SendStaleConfigException(ns, msg, refVersion, currentVersion);
        }
    } else if (refManager) {
        string msg(str::stream() << "not sharded ("
                                 << ((manager.get() == 0) ? string("<none>") : str::stream()
                                             << manager->getSequenceNumber())
                                 << ") but has reference manager ("
                                 << refManager->getSequenceNumber()
                                 << ") "
                                 << "on conn "
                                 << conn->getServerAddress()
                                 << " ("
                                 << conn_in->getServerAddress()
                                 << ")");

        throw SendStaleConfigException(
            ns, msg, refManager->getVersion(shard->getId()), ChunkVersion::UNSHARDED());
    }

    // Has the ChunkManager been reloaded since the last time we updated the shard version over
    // this connection?  If we've never updated the shard version, do so now.
    unsigned long long sequenceNumber = 0;
    if (connectionShardStatus.getSequence(conn, ns, &sequenceNumber)) {
        if (sequenceNumber == officialSequenceNumber) {
            return false;
        }
    }

    ChunkVersion version = ChunkVersion(0, 0, OID());
    if (manager) {
        version = manager->getVersion(shard->getId());
    }

    LOG(1) << "setting shard version of " << version << " for " << ns << " on shard "
           << shard->toString();

    LOG(3) << "last version sent with chunk manager iteration " << sequenceNumber
           << ", current chunk manager iteration is " << officialSequenceNumber;

    BSONObj result;
    if (setShardVersion(txn,
                        conn,
                        ns,
                        grid.shardRegistry()->getConfigServerConnectionString(),
                        version,
                        manager.get(),
                        authoritative,
                        result)) {
        LOG(1) << "      setShardVersion success: " << result;
        connectionShardStatus.setSequence(conn, ns, officialSequenceNumber);
        return true;
    }

    LOG(1) << "       setShardVersion failed!\n" << result;

    if (result["need_authoritative"].trueValue())
        massert(10428, "need_authoritative set but in authoritative mode already", !authoritative);

    if (!authoritative) {
        // use the original connection and get a fresh versionable connection
        // since conn can be invalidated (or worse, freed) after the failure
        checkShardVersion(txn, conn_in, ns, refManager, 1, tryNumber + 1);
        return true;
    }

    if (result["reloadConfig"].trueValue()) {
        if (result["version"].timestampTime() == Date_t()) {
            warning() << "reloading full configuration for " << conf->name()
                      << ", connection state indicates significant version changes";

            // reload db
            conf->reload(txn);
        } else {
            // reload config
            conf->getChunkManager(txn, ns, true);
        }
    }

    const int maxNumTries = 7;
    if (tryNumber < maxNumTries) {
        LOG(tryNumber < (maxNumTries / 2) ? 1 : 0)
            << "going to retry checkShardVersion shard: " << shard->toString() << " " << result;
        sleepmillis(10 * tryNumber);
        // use the original connection and get a fresh versionable connection
        // since conn can be invalidated (or worse, freed) after the failure
        checkShardVersion(txn, conn_in, ns, refManager, true, tryNumber + 1);
        return true;
    }

    string errmsg = str::stream() << "setShardVersion failed shard: " << shard->toString() << " "
                                  << result;
    log() << "     " << errmsg;
    massert(10429, errmsg, 0);
    return true;
}

}  // namespace

// Global version manager
VersionManager versionManager;

void VersionManager::resetShardVersionCB(DBClientBase* conn) {
    connectionShardStatus.reset(conn);
}

bool VersionManager::isVersionableCB(DBClientBase* conn) {
    // We do not version shard connections when issued from mongod
    if (!isMongos()) {
        return false;
    }

    return conn->type() == ConnectionString::MASTER || conn->type() == ConnectionString::SET;
}

bool VersionManager::forceRemoteCheckShardVersionCB(OperationContext* txn, const string& ns) {
    const NamespaceString nss(ns);

    // This will force the database catalog entry to be reloaded
    grid.catalogCache()->invalidate(nss.db().toString());

    auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
    if (!status.isOK()) {
        return false;
    }

    shared_ptr<DBConfig> conf = status.getValue();

    // If we don't have a collection, don't refresh the chunk manager
    if (nsGetCollection(ns).size() == 0) {
        return false;
    }

    ChunkManagerPtr manager = conf->getChunkManagerIfExists(txn, ns, true, true);
    if (!manager) {
        return false;
    }

    return true;
}

bool VersionManager::checkShardVersionCB(OperationContext* txn,
                                         DBClientBase* conn_in,
                                         const string& ns,
                                         bool authoritative,
                                         int tryNumber) {
    return checkShardVersion(txn, conn_in, ns, nullptr, authoritative, tryNumber);
}

bool VersionManager::checkShardVersionCB(OperationContext* txn,
                                         ShardConnection* conn_in,
                                         bool authoritative,
                                         int tryNumber) {
    return checkShardVersion(
        txn, conn_in->get(), conn_in->getNS(), conn_in->getManager(), authoritative, tryNumber);
}

}  // namespace mongo
