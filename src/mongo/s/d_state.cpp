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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/operation_shard_version.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::stringstream;
using std::vector;

namespace {

class UnsetShardingCommand : public Command {
public:
    UnsetShardingCommand() : Command("unsetSharding") {}

    virtual void help(stringstream& help) const {
        help << "internal";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
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

class GetShardVersion : public Command {
public:
    GetShardVersion() : Command("getShardVersion") {}

    virtual void help(stringstream& help) const {
        help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
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

        ShardingState* shardingState = ShardingState::get(txn);

        if (shardingState->enabled()) {
            result.append("configServer", shardingState->getConfigServer(txn));
        } else {
            result.append("configServer", "");
        }

        result.appendTimestamp("global", shardingState->getVersion(ns).toLong());

        ShardedConnectionInfo* const info = ShardedConnectionInfo::get(txn->getClient(), false);
        result.appendBool("inShardedMode", info != NULL);
        if (info) {
            result.appendTimestamp("mine", info->getVersion(ns).toLong());
        } else {
            result.appendTimestamp("mine", 0);
        }

        if (cmdObj["fullMetadata"].trueValue()) {
            shared_ptr<CollectionMetadata> metadata = shardingState->getCollectionMetadata(ns);
            if (metadata) {
                result.append("metadata", metadata->toBSON());
            } else {
                result.append("metadata", BSONObj());
            }
        }

        return true;
    }

} getShardVersion;

class ShardingStateCmd : public Command {
public:
    ShardingStateCmd() : Command("shardingState") {}

    virtual bool isWriteCommandForConfigServer() const {
        return true;
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
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

        ShardingState::get(txn)->appendInfo(txn, result);
        return true;
    }

} shardingStateCmd;

/**
 * @ return true if not in sharded mode
                 or if version for this client is ok
 */
bool shardVersionOk(OperationContext* txn,
                    const string& ns,
                    string& errmsg,
                    ChunkVersion& received,
                    ChunkVersion& wanted) {
    Client* client = txn->getClient();
    ShardingState* shardingState = ShardingState::get(client->getServiceContext());
    if (!shardingState->enabled()) {
        return true;
    }

    if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsToDatabase(ns))) {
        // right now connections to secondaries aren't versioned at all
        return true;
    }

    // If there is a version attached to the OperationContext, use it as the received version.
    // Otherwise, get the received version from the ShardedConnectionInfo.
    if (OperationShardVersion::get(txn).hasShardVersion()) {
        received = OperationShardVersion::get(txn).getShardVersion();
    } else {
        ShardedConnectionInfo* info = ShardedConnectionInfo::get(client, false);
        if (!info) {
            // There is no shard version information on either 'txn' or 'client'. This means that
            // the operation represented by 'txn' is unversioned, and the shard version is always OK
            // for unversioned operations.
            return true;
        }

        if (info->inForceVersionOkMode()) {
            return true;
        }

        received = info->getVersion(ns);
    }

    if (ChunkVersion::isIgnoredVersion(received)) {
        return true;
    }

    wanted = shardingState->getVersion(ns);

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
    MONGO_UNREACHABLE;
}

}  // namespace

ShardForceVersionOkModeBlock::ShardForceVersionOkModeBlock(Client* client) {
    info = ShardedConnectionInfo::get(client, false);
    if (info)
        info->enterForceVersionOkMode();
}

ShardForceVersionOkModeBlock::~ShardForceVersionOkModeBlock() {
    if (info)
        info->leaveForceVersionOkMode();
}

bool haveLocalShardingInfo(Client* client, const string& ns) {
    if (!ShardingState::get(client->getServiceContext())->enabled())
        return false;

    if (!ShardingState::get(client->getServiceContext())->hasVersion(ns))
        return false;

    return ShardedConnectionInfo::get(client, false) != NULL;
}

void ensureShardVersionOKOrThrow(OperationContext* txn, const std::string& ns) {
    string errmsg;
    ChunkVersion received;
    ChunkVersion wanted;
    if (!shardVersionOk(txn, ns, errmsg, received, wanted)) {
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

}  // namespace mongo
