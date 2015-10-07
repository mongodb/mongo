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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/wire_version.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

class SetShardVersion : public Command {
public:
    SetShardVersion() : Command("setShardVersion") {}

    void help(std::stringstream& help) const override {
        help << "internal";
    }

    bool adminOnly() const override {
        return true;
    }

    bool slaveOk() const override {
        return true;
    }

    bool isWriteCommandForConfigServer() const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const std::string&,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        // Steps
        // 1. check basic config
        // 2. extract params from command
        // 3. fast check
        // 4. slow check (LOCKS)

        // Step 1
        Client* client = txn->getClient();
        LastError::get(client).disable();

        ShardingState* shardingState = ShardingState::get(txn);

        const bool authoritative = cmdObj.getBoolField("authoritative");
        const bool noConnectionVersioning = cmdObj.getBoolField("noConnectionVersioning");

        ShardedConnectionInfo dummyInfo;
        ShardedConnectionInfo* info;
        if (noConnectionVersioning) {
            info = &dummyInfo;
        } else {
            info = ShardedConnectionInfo::get(client, true);
        }

        // Check config server is ok or enable sharding
        if (!_checkConfigOrInit(
                txn, cmdObj["configdb"].valuestrsafe(), authoritative, errmsg, result)) {
            return false;
        }

        // check shard name is correct
        if (cmdObj["shard"].type() == String) {
            // The shard host is also sent when using setShardVersion, report this host if there is
            // an error
            shardingState->setShardName(cmdObj["shard"].String());
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
        ChunkVersion version =
            uassertStatusOK(ChunkVersion::parseFromBSONForSetShardVersion(cmdObj));

        // step 3
        const ChunkVersion oldVersion = info->getVersion(ns);
        const ChunkVersion globalVersion = shardingState->getVersion(ns);

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
                while (shardingState->inCriticalMigrateSection()) {
                    log() << "waiting till out of critical section";
                    shardingState->waitTillNotInCriticalSection(10);
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
                while (shardingState->inCriticalMigrateSection()) {
                    log() << "waiting till out of critical section";
                    shardingState->waitTillNotInCriticalSection(10);
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
        Status status = shardingState->refreshMetadataIfNeeded(txn, ns, version, &currVersion);

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

private:
    static bool _checkConfigOrInit(OperationContext* txn,
                                   const string& configdb,
                                   bool authoritative,
                                   string& errmsg,
                                   BSONObjBuilder& result) {
        if (configdb.size() == 0) {
            errmsg = "no configdb";
            return false;
        }

        if (ShardingState::get(txn)->enabled()) {
            auto givenConnStrStatus = ConnectionString::parse(configdb);
            if (!givenConnStrStatus.isOK()) {
                errmsg = str::stream() << "error parsing given config string: " << configdb
                                       << causedBy(givenConnStrStatus.getStatus());
                return false;
            }

            const auto& givenConnStr = givenConnStrStatus.getValue();
            auto storedConnStr = ShardingState::get(txn)->getConfigServer(txn);

            if (givenConnStr.type() == ConnectionString::SET &&
                storedConnStr.type() == ConnectionString::SET) {
                if (givenConnStr.getSetName() != storedConnStr.getSetName()) {
                    errmsg = str::stream()
                        << "given config server set name: " << givenConnStr.getSetName()
                        << " differs from known set name: " << storedConnStr.getSetName();

                    return false;
                }

                return true;
            }

            const auto& storedRawConfigString = storedConnStr.toString();
            if (storedRawConfigString == configdb) {
                return true;
            }

            result.append("configdb",
                          BSON("stored" << storedRawConfigString << "given" << configdb));

            errmsg = str::stream() << "mongos specified a different config database string : "
                                   << "stored : " << storedRawConfigString
                                   << " vs given : " << configdb;
            return false;
        }

        if (!authoritative) {
            result.appendBool("need_authoritative", true);
            errmsg = "first setShardVersion";
            return false;
        }

        ShardingState::get(txn)->initialize(txn, configdb);
        return true;
    }

} setShardVersionCmd;

}  // namespace
}  // namespace mongo
