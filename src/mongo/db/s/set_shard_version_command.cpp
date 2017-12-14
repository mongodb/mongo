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
#include "mongo/db/catalog/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

using std::string;
using str::stream;

namespace {

class SetShardVersion : public ErrmsgCommandDeprecated {
public:
    SetShardVersion() : ErrmsgCommandDeprecated("setShardVersion") {}

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

    bool errmsgRun(OperationContext* opCtx,
                   const std::string&,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        uassert(ErrorCodes::IllegalOperation,
                "can't issue setShardVersion from 'eval'",
                !opCtx->getClient()->isInDirectClient());

        auto const shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        // Steps
        // 1. As long as the command does not have noConnectionVersioning set, register a
        //    ShardedConnectionInfo for this client connection (this is for clients using
        //    ShardConnection). Registering the ShardedConnectionInfo guarantees that we will check
        //    the shardVersion on all requests from this client connection. The connection's version
        //    will be updated on each subsequent setShardVersion sent on this connection.
        //
        // 2. If we have received the init form of setShardVersion, vacuously return true.
        //    The init form of setShardVersion was used to initialize sharding awareness on a shard,
        //    but was made obsolete in v3.4 by making nodes sharding-aware when they are added to a
        //    cluster. The init form was kept in v3.4 shards for compatibility with mixed-version
        //    3.2/3.4 clusters, but was deprecated and made to vacuously return true in v3.6.
        //
        // 3. Validate all command parameters against the info in our ShardingState, and return an
        //    error if they do not match.
        //
        // 4. If the sent shardVersion is compatible with our shardVersion, update the shardVersion
        //    in this client's ShardedConnectionInfo if needed.
        //
        // 5. If the sent shardVersion indicates a drop, jump to step 7.
        //
        // 6. If the sent shardVersion is staler than ours, return a stale config error.
        //
        // 7. If the sent shardVersion is newer than ours (or indicates a drop), reload our metadata
        //    and compare the sent shardVersion with what we reloaded. If the versions are now
        //    compatible, update the shardVersion in this client's ShardedConnectionInfo, as in
        //    step 4. If the sent shardVersion is staler than what we reloaded, return a stale
        //    config error, as in step 6.

        // Step 1

        Client* client = opCtx->getClient();
        LastError::get(client).disable();

        const bool authoritative = cmdObj.getBoolField("authoritative");
        const bool noConnectionVersioning = cmdObj.getBoolField("noConnectionVersioning");

        ShardedConnectionInfo dummyInfo;
        ShardedConnectionInfo* info;
        if (noConnectionVersioning) {
            info = &dummyInfo;
        } else {
            info = ShardedConnectionInfo::get(client, true);
        }

        // Step 2

        // The init form of setShardVersion was deprecated in v3.6. For backwards compatibility with
        // pre-v3.6 mongos, return true.
        const auto isInit = cmdObj["init"].trueValue();
        if (isInit) {
            result.append("initialized", true);
            return true;
        }

        // Step 3

        // Validate shardName parameter.
        const auto shardName = cmdObj["shard"].str();
        const auto storedShardName = ShardingState::get(opCtx)->getShardName();
        uassert(ErrorCodes::BadValue,
                str::stream() << "received shardName " << shardName
                              << " which differs from stored shardName "
                              << storedShardName,
                storedShardName == shardName);

        // Validate config connection string parameter.
        const auto configdb = cmdObj["configdb"].String();
        uassert(ErrorCodes::BadValue,
                "Config server connection string cannot be empty",
                !configdb.empty());

        const auto givenConnStr = uassertStatusOK(ConnectionString::parse(configdb));
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Given config server string " << givenConnStr.toString()
                              << " is not of type SET",
                givenConnStr.type() == ConnectionString::SET);

        const auto storedConnStr = ShardingState::get(opCtx)->getConfigServer(opCtx);
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Given config server set name: " << givenConnStr.getSetName()
                              << " differs from known set name: "
                              << storedConnStr.getSetName(),
                givenConnStr.getSetName() == storedConnStr.getSetName());

        // Validate namespace parameter.
        const NamespaceString nss(cmdObj["setShardVersion"].String());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace " << nss.ns(),
                nss.isValid());

        // Validate chunk version parameter.
        const ChunkVersion requestedVersion =
            uassertStatusOK(ChunkVersion::parseFromBSONForSetShardVersion(cmdObj));

        // Step 4

        const ChunkVersion connectionVersion = info->getVersion(nss.ns());
        connectionVersion.addToBSON(result, "oldVersion");

        {
            boost::optional<AutoGetDb> autoDb;
            autoDb.emplace(opCtx, nss.db(), MODE_IS);

            // Slave nodes cannot support set shard version
            uassert(ErrorCodes::NotMaster,
                    str::stream() << "setShardVersion with collection version is only supported "
                                     "against primary nodes, but it was received for namespace "
                                  << nss.ns(),
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                         nss.db()));

            // Views do not require a shard version check.
            if (autoDb->getDb() && !autoDb->getDb()->getCollection(opCtx, nss) &&
                autoDb->getDb()->getViewCatalog()->lookup(opCtx, nss.ns())) {
                return true;
            }

            boost::optional<Lock::CollectionLock> collLock;
            collLock.emplace(opCtx->lockState(), nss.ns(), MODE_IS);

            auto css = CollectionShardingState::get(opCtx, nss);
            const ChunkVersion collectionShardVersion =
                (css->getMetadata() ? css->getMetadata()->getShardVersion()
                                    : ChunkVersion::UNSHARDED());

            if (requestedVersion.isWriteCompatibleWith(collectionShardVersion)) {
                // mongos and mongod agree!
                // Now we should update the connection's version if it's not compatible with the
                // request's version. This could happen if the shard's metadata has changed, but
                // the remote client has already refreshed its view of the metadata since the last
                // time it sent anything over this connection.
                if (!connectionVersion.isWriteCompatibleWith(requestedVersion)) {
                    // A migration occurred.
                    if (connectionVersion < collectionShardVersion &&
                        connectionVersion.epoch() == collectionShardVersion.epoch()) {
                        info->setVersion(nss.ns(), requestedVersion);
                    }
                    // The collection was dropped and recreated.
                    else if (authoritative) {
                        info->setVersion(nss.ns(), requestedVersion);
                    } else {
                        result.append("ns", nss.ns());
                        result.appendBool("need_authoritative", true);
                        errmsg = str::stream() << "verifying drop on '" << nss.ns() << "'";
                        return false;
                    }
                }

                return true;
            }

            // Step 5

            const bool isDropRequested =
                !requestedVersion.isSet() && collectionShardVersion.isSet();

            if (isDropRequested) {
                if (!authoritative) {
                    result.appendBool("need_authoritative", true);
                    result.append("ns", nss.ns());
                    collectionShardVersion.addToBSON(result, "globalVersion");
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }

                // Fall through to metadata reload below
            } else {
                // Not Dropping

                // Step 6

                // TODO: Refactor all of this
                if (requestedVersion < connectionVersion &&
                    requestedVersion.epoch() == connectionVersion.epoch()) {
                    errmsg = str::stream() << "this connection already had a newer version "
                                           << "of collection '" << nss.ns() << "'";
                    result.append("ns", nss.ns());
                    requestedVersion.addToBSON(result, "newVersion");
                    collectionShardVersion.addToBSON(result, "globalVersion");
                    return false;
                }

                // TODO: Refactor all of this
                if (requestedVersion < collectionShardVersion &&
                    requestedVersion.epoch() == collectionShardVersion.epoch()) {
                    if (css->getMigrationSourceManager()) {
                        auto critSecSignal =
                            css->getMigrationSourceManager()->getMigrationCriticalSectionSignal(
                                false);
                        if (critSecSignal) {
                            collLock.reset();
                            autoDb.reset();
                            log() << "waiting till out of critical section";
                            critSecSignal->waitFor(opCtx, Seconds(10));
                        }
                    }

                    errmsg = str::stream() << "shard global version for collection is higher "
                                           << "than trying to set to '" << nss.ns() << "'";
                    result.append("ns", nss.ns());
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
                            css->getMigrationSourceManager()->getMigrationCriticalSectionSignal(
                                false);
                        if (critSecSignal) {
                            collLock.reset();
                            autoDb.reset();
                            log() << "waiting till out of critical section";
                            critSecSignal->waitFor(opCtx, Seconds(10));
                        }
                    }

                    // need authoritative for first look
                    result.append("ns", nss.ns());
                    result.appendBool("need_authoritative", true);
                    errmsg = str::stream() << "first time for collection '" << nss.ns() << "'";
                    return false;
                }

                // Fall through to metadata reload below
            }
        }

        // Step 7

        Status status = shardingState->onStaleShardVersion(opCtx, nss, requestedVersion);

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IS);

            ChunkVersion currVersion = ChunkVersion::UNSHARDED();
            auto collMetadata = CollectionShardingState::get(opCtx, nss)->getMetadata();
            if (collMetadata) {
                currVersion = collMetadata->getShardVersion();
            }

            if (!status.isOK()) {
                // The reload itself was interrupted or confused here

                errmsg = str::stream() << "could not refresh metadata for " << nss.ns()
                                       << " with requested shard version "
                                       << requestedVersion.toString()
                                       << ", stored shard version is " << currVersion.toString()
                                       << causedBy(redact(status));

                warning() << errmsg;

                result.append("ns", nss.ns());
                requestedVersion.addToBSON(result, "version");
                currVersion.addToBSON(result, "globalVersion");
                result.appendBool("reloadConfig", true);

                return false;
            } else if (!requestedVersion.isWriteCompatibleWith(currVersion)) {
                // We reloaded a version that doesn't match the version mongos was trying to
                // set.
                errmsg = str::stream() << "requested shard version differs from"
                                       << " config shard version for " << nss.ns()
                                       << ", requested version is " << requestedVersion.toString()
                                       << " but found version " << currVersion.toString();

                OCCASIONALLY warning() << errmsg;

                // WARNING: the exact fields below are important for compatibility with mongos
                // version reload.

                result.append("ns", nss.ns());
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

        info->setVersion(nss.ns(), requestedVersion);
        return true;
    }

} setShardVersionCmd;

}  // namespace
}  // namespace mongo
