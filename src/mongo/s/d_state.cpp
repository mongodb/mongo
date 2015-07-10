/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/d_state.h"

#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/wire_version.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::stringstream;
using std::vector;

ShardForceVersionOkModeBlock::ShardForceVersionOkModeBlock(Client* client) {
    info = ShardedConnectionInfo::get(client, false);
    if (info)
        info->enterForceVersionOkMode();
}

ShardForceVersionOkModeBlock::~ShardForceVersionOkModeBlock() {
    if (info)
        info->leaveForceVersionOkMode();
}

class MongodShardCommand : public Command {
public:
    MongodShardCommand(const char* n) : Command(n) {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
};


bool haveLocalShardingInfo(Client* client, const string& ns) {
    if (!shardingState.enabled())
        return false;

    if (!shardingState.hasVersion(ns))
        return false;

    return ShardedConnectionInfo::get(client, false) != NULL;
}

class UnsetShardingCommand : public MongodShardCommand {
public:
    UnsetShardingCommand() : MongodShardCommand("unsetSharding") {}

    virtual void help(stringstream& help) const {
        help << "internal";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual bool slaveOk() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardedConnectionInfo::reset(txn->getClient());
        return true;
    }

} unsetShardingCommand;

class SetShardVersion : public MongodShardCommand {
public:
    SetShardVersion() : MongodShardCommand("setShardVersion") {}

    virtual void help(stringstream& help) const {
        help << "internal";
    }

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool checkConfigOrInit(OperationContext* txn,
                           const string& configdb,
                           bool authoritative,
                           string& errmsg,
                           BSONObjBuilder& result,
                           bool locked = false) const {
        if (configdb.size() == 0) {
            errmsg = "no configdb";
            return false;
        }

        if (shardingState.enabled()) {
            if (configdb == shardingState.getConfigServer())
                return true;

            result.append("configdb",
                          BSON("stored" << shardingState.getConfigServer() << "given" << configdb));

            errmsg = str::stream() << "mongos specified a different config database string : "
                                   << "stored : " << shardingState.getConfigServer()
                                   << " vs given : " << configdb;
            return false;
        }

        if (!authoritative) {
            result.appendBool("need_authoritative", true);
            errmsg = "first setShardVersion";
            return false;
        }

        if (locked) {
            shardingState.initialize(configdb);
            return true;
        }

        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite lk(txn->lockState());
        return checkConfigOrInit(txn, configdb, authoritative, errmsg, result, true);
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        // Compatibility error for < v3.0 mongoses still active in the cluster
        // TODO: Remove post-3.0
        if (!cmdObj["serverID"].eoo()) {
            // This mongos is too old to talk to us
            string errMsg = stream() << "v3.0 mongod is incompatible with v2.6 mongos, "
                                     << "a v2.6 mongos may be running in the v3.0 cluster at "
                                     << txn->getClient()->clientAddress(false);
            error() << errMsg;
            return appendCommandStatus(result, Status(ErrorCodes::ProtocolError, errMsg));
        }

        // Steps
        // 1. check basic config
        // 2. extract params from command
        // 3. fast check
        // 4. slow check (LOCKS)

        // step 1

        Client* client = txn->getClient();
        LastError::get(client).disable();
        ShardedConnectionInfo* info = ShardedConnectionInfo::get(client, true);

        bool authoritative = cmdObj.getBoolField("authoritative");

        // check config server is ok or enable sharding
        if (!checkConfigOrInit(
                txn, cmdObj["configdb"].valuestrsafe(), authoritative, errmsg, result)) {
            return false;
        }

        // check shard name is correct
        if (cmdObj["shard"].type() == String) {
            // The shard host is also sent when using setShardVersion, report this host if there
            // is an error.
            shardingState.gotShardNameAndHost(cmdObj["shard"].String(), cmdObj["shardHost"].str());
        }

        // Handle initial shard connection
        if (cmdObj["version"].eoo() && cmdObj["init"].trueValue()) {
            result.append("initialized", true);

            // Send back wire version to let mongos know what protocol we can speak
            result.append("minWireVersion", minWireVersion);
            result.append("maxWireVersion", maxWireVersion);

            return true;
        }

        string ns = cmdObj["setShardVersion"].valuestrsafe();
        if (ns.size() == 0) {
            errmsg = "need to specify namespace";
            return false;
        }


        // we can run on a slave up to here
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                nsToDatabase(ns))) {
            result.append("errmsg", "not master");
            result.append("note", "from post init in setShardVersion");
            return false;
        }

        // step 2
        if (!ChunkVersion::canParseBSON(cmdObj, "version")) {
            errmsg = "need to specify version";
            return false;
        }

        const ChunkVersion version = ChunkVersion::fromBSON(cmdObj, "version");

        // step 3

        const ChunkVersion oldVersion = info->getVersion(ns);
        const ChunkVersion globalVersion = shardingState.getVersion(ns);

        oldVersion.addToBSON(result, "oldVersion");

        if (version.isWriteCompatibleWith(globalVersion)) {
            // mongos and mongod agree!
            if (!oldVersion.isWriteCompatibleWith(version)) {
                if (oldVersion < globalVersion && oldVersion.hasEqualEpoch(globalVersion)) {
                    info->setVersion(ns, version);
                } else if (authoritative) {
                    // this means there was a drop and our version is reset
                    info->setVersion(ns, version);
                } else {
                    result.append("ns", ns);
                    result.appendBool("need_authoritative", true);
                    errmsg = "verifying drop on '" + ns + "'";
                    return false;
                }
            }

            return true;
        }

        // step 4
        // Cases below all either return OR fall-through to remote metadata reload.
        const bool isDropRequested = !version.isSet() && globalVersion.isSet();

        if (isDropRequested) {
            if (!authoritative) {
                result.appendBool("need_authoritative", true);
                result.append("ns", ns);
                globalVersion.addToBSON(result, "globalVersion");
                errmsg = "dropping needs to be authoritative";
                return false;
            }

            // Fall through to metadata reload below
        } else {
            // Not Dropping

            // TODO: Refactor all of this
            if (version < oldVersion && version.hasEqualEpoch(oldVersion)) {
                errmsg = str::stream() << "this connection already had a newer version "
                                       << "of collection '" << ns << "'";
                result.append("ns", ns);
                version.addToBSON(result, "newVersion");
                globalVersion.addToBSON(result, "globalVersion");
                return false;
            }

            // TODO: Refactor all of this
            if (version < globalVersion && version.hasEqualEpoch(globalVersion)) {
                while (shardingState.inCriticalMigrateSection()) {
                    log() << "waiting till out of critical section";
                    shardingState.waitTillNotInCriticalSection(10);
                }
                errmsg = str::stream() << "shard global version for collection is higher "
                                       << "than trying to set to '" << ns << "'";
                result.append("ns", ns);
                version.addToBSON(result, "version");
                globalVersion.addToBSON(result, "globalVersion");
                result.appendBool("reloadConfig", true);
                return false;
            }

            if (!globalVersion.isSet() && !authoritative) {
                // Needed b/c when the last chunk is moved off a shard,
                // the version gets reset to zero, which should require a reload.
                while (shardingState.inCriticalMigrateSection()) {
                    log() << "waiting till out of critical section";
                    shardingState.waitTillNotInCriticalSection(10);
                }

                // need authoritative for first look
                result.append("ns", ns);
                result.appendBool("need_authoritative", true);
                errmsg = "first time for collection '" + ns + "'";
                return false;
            }

            // Fall through to metadata reload below
        }

        ChunkVersion currVersion;
        Status status = shardingState.refreshMetadataIfNeeded(txn, ns, version, &currVersion);

        if (!status.isOK()) {
            // The reload itself was interrupted or confused here

            errmsg = str::stream() << "could not refresh metadata for " << ns
                                   << " with requested shard version " << version.toString()
                                   << ", stored shard version is " << currVersion.toString()
                                   << causedBy(status.reason());

            warning() << errmsg;

            result.append("ns", ns);
            version.addToBSON(result, "version");
            currVersion.addToBSON(result, "globalVersion");
            result.appendBool("reloadConfig", true);

            return false;
        } else if (!version.isWriteCompatibleWith(currVersion)) {
            // We reloaded a version that doesn't match the version mongos was trying to
            // set.

            errmsg = str::stream() << "requested shard version differs from"
                                   << " config shard version for " << ns
                                   << ", requested version is " << version.toString()
                                   << " but found version " << currVersion.toString();

            OCCASIONALLY warning() << errmsg;

            // WARNING: the exact fields below are important for compatibility with mongos
            // version reload.

            result.append("ns", ns);
            currVersion.addToBSON(result, "globalVersion");

            // If this was a reset of a collection or the last chunk moved out, inform mongos to
            // do a full reload.
            if (currVersion.epoch() != version.epoch() || !currVersion.isSet()) {
                result.appendBool("reloadConfig", true);
                // Zero-version also needed to trigger full mongos reload, sadly
                // TODO: Make this saner, and less impactful (full reload on last chunk is bad)
                ChunkVersion(0, 0, OID()).addToBSON(result, "version");
                // For debugging
                version.addToBSON(result, "origVersion");
            } else {
                version.addToBSON(result, "version");
            }

            return false;
        }

        info->setVersion(ns, version);
        return true;
    }

} setShardVersionCmd;

class GetShardVersion : public MongodShardCommand {
public:
    GetShardVersion() : MongodShardCommand("getShardVersion") {}

    virtual void help(stringstream& help) const {
        help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string ns = cmdObj["getShardVersion"].valuestrsafe();
        if (ns.size() == 0) {
            errmsg = "need to specify full namespace";
            return false;
        }

        if (shardingState.enabled()) {
            result.append("configServer", shardingState.getConfigServer());
        } else {
            result.append("configServer", "");
        }

        result.appendTimestamp("global", shardingState.getVersion(ns).toLong());

        ShardedConnectionInfo* const info = ShardedConnectionInfo::get(txn->getClient(), false);
        result.appendBool("inShardedMode", info != NULL);
        if (info) {
            result.appendTimestamp("mine", info->getVersion(ns).toLong());
        } else {
            result.appendTimestamp("mine", 0);
        }

        if (cmdObj["fullMetadata"].trueValue()) {
            shared_ptr<CollectionMetadata> metadata = shardingState.getCollectionMetadata(ns);
            if (metadata) {
                result.append("metadata", metadata->toBSON());
            } else {
                result.append("metadata", BSONObj());
            }
        }

        return true;
    }

} getShardVersion;

class ShardingStateCmd : public MongodShardCommand {
public:
    ShardingStateCmd() : MongodShardCommand("shardingState") {}

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::shardingState);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
        OldClientContext ctx(txn, dbname);

        shardingState.appendInfo(result);
        return true;
    }

} shardingStateCmd;

/**
 * @ return true if not in sharded mode
                 or if version for this client is ok
 */
static bool shardVersionOk(Client* client,
                           const string& ns,
                           string& errmsg,
                           ChunkVersion& received,
                           ChunkVersion& wanted) {
    if (!shardingState.enabled())
        return true;

    if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsToDatabase(ns))) {
        // right now connections to secondaries aren't versioned at all
        return true;
    }

    ShardedConnectionInfo* info = ShardedConnectionInfo::get(client, false);

    if (!info) {
        // this means the client has nothing sharded
        // so this allows direct connections to do whatever they want
        // which i think is the correct behavior
        return true;
    }

    if (info->inForceVersionOkMode()) {
        return true;
    }

    // TODO : all collections at some point, be sharded or not, will have a version
    //  (and a CollectionMetadata)
    received = info->getVersion(ns);

    if (ChunkVersion::isIgnoredVersion(received)) {
        return true;
    }

    wanted = shardingState.getVersion(ns);

    if (received.isWriteCompatibleWith(wanted))
        return true;

    //
    // Figure out exactly why not compatible, send appropriate error message
    // The versions themselves are returned in the error, so not needed in messages here
    //

    // Check epoch first, to send more meaningful message, since other parameters probably
    // won't match either
    if (!wanted.hasEqualEpoch(received)) {
        errmsg = str::stream() << "version epoch mismatch detected for " << ns << ", "
                               << "the collection may have been dropped and recreated";
        return false;
    }

    if (!wanted.isSet() && received.isSet()) {
        errmsg = str::stream() << "this shard no longer contains chunks for " << ns << ", "
                               << "the collection may have been dropped";
        return false;
    }

    if (wanted.isSet() && !received.isSet()) {
        errmsg = str::stream() << "this shard contains versioned chunks for " << ns << ", "
                               << "but no version set in request";
        return false;
    }

    if (wanted.majorVersion() != received.majorVersion()) {
        //
        // Could be > or < - wanted is > if this is the source of a migration,
        // wanted < if this is the target of a migration
        //

        errmsg = str::stream() << "version mismatch detected for " << ns << ", "
                               << "stored major version " << wanted.majorVersion()
                               << " does not match received " << received.majorVersion();
        return false;
    }

    // Those are all the reasons the versions can mismatch
    verify(false);

    return false;
}

void ensureShardVersionOKOrThrow(Client* client, const std::string& ns) {
    string errmsg;
    ChunkVersion received;
    ChunkVersion wanted;
    if (!shardVersionOk(client, ns, errmsg, received, wanted)) {
        StringBuilder sb;
        sb << "[" << ns << "] shard version not ok: " << errmsg;
        throw SendStaleConfigException(ns, sb.str(), received, wanted);
    }
}

void usingAShardConnection(const string& addr) {}

void saveGLEStats(const BSONObj& result, StringData hostString) {
    // Declared in cluster_last_error_info.h.
    //
    // This can be called in mongod, which is unfortunate.  To fix this,
    // we can redesign how connection pooling works on mongod for sharded operations.
}
}
