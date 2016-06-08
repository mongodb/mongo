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
#include "mongo/db/db_raii.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_source_manager.h"
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

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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

        const auto configDBStr = cmdObj["configdb"].str();
        string shardName = cmdObj["shard"].str();
        const auto isInit = cmdObj["init"].trueValue();

        const string ns = cmdObj["setShardVersion"].valuestrsafe();
        if (shardName.empty()) {
            if (isInit && ns.empty()) {
                // Note: v3.0 mongos ConfigCoordinator doesn't set the shard field when sending
                // setShardVersion to config.
                shardName = "config";
            } else {
                errmsg = "shard name cannot be empty if not init";
                return false;
            }
        }

        // check shard name is correct
        // The shard host is also sent when using setShardVersion, report this host if there is
        // an error or mismatch.
        shardingState->setShardName(shardName);

        if (!_checkConfigOrInit(txn, configDBStr, shardName, authoritative, errmsg, result)) {
            return false;
        }

        // Handle initial shard connection
        if (cmdObj["version"].eoo() && isInit) {
            result.append("initialized", true);

            // TODO: SERVER-21397 remove post v3.3.
            // Send back wire version to let mongos know what protocol we can speak
            result.append("minWireVersion", WireSpec::instance().minWireVersionIncoming);
            result.append("maxWireVersion", WireSpec::instance().maxWireVersionIncoming);

            return true;
        }

        if (ns.size() == 0) {
            errmsg = "need to specify namespace";
            return false;
        }

        const NamespaceString nss(ns);

        // Backwards compatibility for SERVER-23119
        if (!nss.isValid()) {
            warning() << "Invalid namespace used for setShardVersion: " << ns;
            return true;
        }

        // we can run on a slave up to here
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nss.db())) {
            result.append("errmsg", "not master");
            result.append("note", "from post init in setShardVersion");
            return false;
        }

        // step 2
        const ChunkVersion requestedVersion =
            uassertStatusOK(ChunkVersion::parseFromBSONForSetShardVersion(cmdObj));

        // step 3 - Actual version checking
        const ChunkVersion connectionVersion = info->getVersion(ns);
        connectionVersion.addToBSON(result, "oldVersion");

        {
            // Use a stable collection metadata while performing the checks
            boost::optional<AutoGetCollection> autoColl;
            autoColl.emplace(txn, nss, MODE_IS);

            auto css = CollectionShardingState::get(txn, nss);
            const ChunkVersion collectionShardVersion =
                (css->getMetadata() ? css->getMetadata()->getShardVersion()
                                    : ChunkVersion::UNSHARDED());

            if (requestedVersion.isWriteCompatibleWith(collectionShardVersion)) {
                // mongos and mongod agree!
                if (!connectionVersion.isWriteCompatibleWith(requestedVersion)) {
                    if (connectionVersion < collectionShardVersion &&
                        connectionVersion.epoch() == collectionShardVersion.epoch()) {
                        info->setVersion(ns, requestedVersion);
                    } else if (authoritative) {
                        // this means there was a drop and our version is reset
                        info->setVersion(ns, requestedVersion);
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
            const bool isDropRequested =
                !requestedVersion.isSet() && collectionShardVersion.isSet();

            if (isDropRequested) {
                if (!authoritative) {
                    result.appendBool("need_authoritative", true);
                    result.append("ns", ns);
                    collectionShardVersion.addToBSON(result, "globalVersion");
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }

                // Fall through to metadata reload below
            } else {
                // Not Dropping

                // TODO: Refactor all of this
                if (requestedVersion < connectionVersion &&
                    requestedVersion.epoch() == connectionVersion.epoch()) {
                    errmsg = str::stream() << "this connection already had a newer version "
                                           << "of collection '" << ns << "'";
                    result.append("ns", ns);
                    requestedVersion.addToBSON(result, "newVersion");
                    collectionShardVersion.addToBSON(result, "globalVersion");
                    return false;
                }

                // TODO: Refactor all of this
                if (requestedVersion < collectionShardVersion &&
                    requestedVersion.epoch() == collectionShardVersion.epoch()) {
                    if (css->getMigrationSourceManager()) {
                        auto critSecSignal =
                            css->getMigrationSourceManager()->getMigrationCriticalSectionSignal();
                        if (critSecSignal) {
                            autoColl.reset();
                            log() << "waiting till out of critical section";
                            critSecSignal->waitFor(txn, Seconds(10));
                        }
                    }

                    errmsg = str::stream() << "shard global version for collection is higher "
                                           << "than trying to set to '" << ns << "'";
                    result.append("ns", ns);
                    requestedVersion.addToBSON(result, "version");
                    collectionShardVersion.addToBSON(result, "globalVersion");
                    result.appendBool("reloadConfig", true);
                    return false;
                }

                if (!collectionShardVersion.isSet() && !authoritative) {
                    // Needed b/c when the last chunk is moved off a shard, the version gets reset
                    // to zero, which should require a reload.
                    if (css->getMigrationSourceManager()) {
                        auto critSecSignal =
                            css->getMigrationSourceManager()->getMigrationCriticalSectionSignal();
                        if (critSecSignal) {
                            autoColl.reset();
                            log() << "waiting till out of critical section";
                            critSecSignal->waitFor(txn, Seconds(10));
                        }
                    }

                    // need authoritative for first look
                    result.append("ns", ns);
                    result.appendBool("need_authoritative", true);
                    errmsg = "first time for collection '" + ns + "'";
                    return false;
                }

                // Fall through to metadata reload below
            }
        }

        Status status = shardingState->onStaleShardVersion(txn, nss, requestedVersion);

        {
            AutoGetCollection autoColl(txn, nss, MODE_IS);

            ChunkVersion currVersion = ChunkVersion::UNSHARDED();
            auto collMetadata = CollectionShardingState::get(txn, nss)->getMetadata();
            if (collMetadata) {
                currVersion = collMetadata->getShardVersion();
            }

            if (!status.isOK()) {
                // The reload itself was interrupted or confused here

                errmsg = str::stream()
                    << "could not refresh metadata for " << ns << " with requested shard version "
                    << requestedVersion.toString() << ", stored shard version is "
                    << currVersion.toString() << causedBy(status.reason());

                warning() << errmsg;

                result.append("ns", ns);
                requestedVersion.addToBSON(result, "version");
                currVersion.addToBSON(result, "globalVersion");
                result.appendBool("reloadConfig", true);

                return false;
            } else if (!requestedVersion.isWriteCompatibleWith(currVersion)) {
                // We reloaded a version that doesn't match the version mongos was trying to
                // set.
                errmsg = str::stream() << "requested shard version differs from"
                                       << " config shard version for " << ns
                                       << ", requested version is " << requestedVersion.toString()
                                       << " but found version " << currVersion.toString();

                OCCASIONALLY warning() << errmsg;

                // WARNING: the exact fields below are important for compatibility with mongos
                // version reload.

                result.append("ns", ns);
                currVersion.addToBSON(result, "globalVersion");

                // If this was a reset of a collection or the last chunk moved out, inform mongos to
                // do a full reload.
                if (currVersion.epoch() != requestedVersion.epoch() || !currVersion.isSet()) {
                    result.appendBool("reloadConfig", true);
                    // Zero-version also needed to trigger full mongos reload, sadly
                    // TODO: Make this saner, and less impactful (full reload on last chunk is bad)
                    ChunkVersion(0, 0, OID()).addToBSON(result, "version");
                    // For debugging
                    requestedVersion.addToBSON(result, "origVersion");
                } else {
                    requestedVersion.addToBSON(result, "version");
                }

                return false;
            }
        }

        info->setVersion(ns, requestedVersion);
        return true;
    }

private:
    /**
     * Checks if this server has already been initialized. If yes, then checks that the configdb
     * settings matches the initialized settings. Otherwise, initializes the server with the given
     * settings.
     */
    bool _checkConfigOrInit(OperationContext* txn,
                            const string& configdb,
                            const string& shardName,
                            bool authoritative,
                            string& errmsg,
                            BSONObjBuilder& result) {
        if (configdb.size() == 0) {
            errmsg = "no configdb";
            return false;
        }

        auto givenConnStrStatus = ConnectionString::parse(configdb);
        if (!givenConnStrStatus.isOK()) {
            errmsg = str::stream() << "error parsing given config string: " << configdb
                                   << causedBy(givenConnStrStatus.getStatus());
            return false;
        }

        const auto& givenConnStr = givenConnStrStatus.getValue();
        ConnectionString storedConnStr;

        if (shardName == "config") {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (!_configStr.isValid()) {
                _configStr = givenConnStr;
                return true;
            } else {
                storedConnStr = _configStr;
            }
        } else if (ShardingState::get(txn)->enabled()) {
            invariant(!_configStr.isValid());
            storedConnStr = ShardingState::get(txn)->getConfigServer(txn);
        }

        if (storedConnStr.isValid()) {
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

        invariant(shardName != "config");

        if (!authoritative) {
            result.appendBool("need_authoritative", true);
            errmsg = "first setShardVersion";
            return false;
        }

        ShardingState::get(txn)->initializeFromConfigConnString(txn, configdb);
        return true;
    }

    // Only for servers that are running as a config server.
    stdx::mutex _mutex;
    ConnectionString _configStr;

} setShardVersionCmd;

}  // namespace
}  // namespace mongo
