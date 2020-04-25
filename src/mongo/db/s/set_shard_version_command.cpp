/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

class SetShardVersion : public ErrmsgCommandDeprecated {
public:
    SetShardVersion() : ErrmsgCommandDeprecated("setShardVersion") {}

    std::string help() const override {
        return "internal";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string&,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) {
        uassert(ErrorCodes::IllegalOperation,
                "can't issue setShardVersion from 'eval'",
                !opCtx->getClient()->isInDirectClient());

        auto const shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        // Steps
        // 1. Set the `authoritative` and `forceRefresh` variables from the command object.
        //
        // 2. Validate all command parameters against the info in our ShardingState, and return an
        //    error if they do not match.
        //
        // 3. If the sent shardVersion is compatible with our shardVersion, return.
        //
        // 4. If the sent shardVersion indicates a drop, jump to step 6.
        //
        // 5. If the sent shardVersion is staler than ours, return a stale config error.
        //
        // 6. If the sent shardVersion is newer than ours (or indicates a drop), reload our metadata
        //    and compare the sent shardVersion with what we reloaded. If the sent shardVersion is
        //    staler than what we reloaded, return a stale config error, as in step 5.

        // Step 1

        Client* client = opCtx->getClient();
        LastError::get(client).disable();

        const bool authoritative = cmdObj.getBoolField("authoritative");
        // A flag that specifies whether the set shard version catalog refresh
        // is allowed to join an in-progress refresh triggered by an other
        // thread, or whether it's required to either a) trigger its own
        // refresh or b) wait for a refresh to be started after it has entered the
        // getCollectionRoutingInfoWithRefresh function
        const bool forceRefresh = cmdObj.getBoolField("forceRefresh");

        // Step 2

        // Validate shardName parameter.
        const auto shardName = cmdObj["shard"].str();
        const auto storedShardName = shardingState->shardId().toString();
        uassert(ErrorCodes::BadValue,
                str::stream() << "received shardName " << shardName
                              << " which differs from stored shardName " << storedShardName,
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

        const auto storedConnStr =
            Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString();
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Given config server set name: " << givenConnStr.getSetName()
                              << " differs from known set name: " << storedConnStr.getSetName(),
                givenConnStr.getSetName() == storedConnStr.getSetName());

        // Validate namespace parameter.
        const NamespaceString nss(cmdObj["setShardVersion"].String());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace " << nss.ns(),
                nss.isValid());

        // Validate chunk version parameter.
        const ChunkVersion requestedVersion = uassertStatusOK(
            ChunkVersion::parseLegacyWithField(cmdObj, SetShardVersionRequest::kVersion));

        // Step 3

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

            boost::optional<Lock::CollectionLock> collLock;
            collLock.emplace(opCtx, nss, MODE_IS);

            // Views do not require a shard version check. We do not care about invalid system views
            // for this check, only to validate if a view already exists for this namespace.
            if (autoDb->getDb() &&
                !CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss) &&
                ViewCatalog::get(autoDb->getDb())
                    ->lookupWithoutValidatingDurableViews(opCtx, nss.ns())) {
                return true;
            }

            auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
            const ChunkVersion collectionShardVersion = [&] {
                auto optMetadata = csr->getCurrentMetadataIfKnown();
                return (optMetadata && optMetadata->isSharded()) ? optMetadata->getShardVersion()
                                                                 : ChunkVersion::UNSHARDED();
            }();

            if (requestedVersion.isWriteCompatibleWith(collectionShardVersion)) {
                return true;
            }

            // Step 4

            const bool isDropRequested =
                !requestedVersion.isSet() && collectionShardVersion.isSet();

            if (isDropRequested) {
                if (!authoritative) {
                    result.appendBool("need_authoritative", true);
                    result.append("ns", nss.ns());
                    collectionShardVersion.appendLegacyWithField(&result, "globalVersion");
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }

                // Fall through to metadata reload below
            } else {
                // Not Dropping

                // Step 5

                // TODO: Refactor all of this
                if (requestedVersion < collectionShardVersion &&
                    requestedVersion.epoch() == collectionShardVersion.epoch()) {
                    auto critSecSignal = csr->getCriticalSectionSignal(
                        opCtx, ShardingMigrationCriticalSection::kWrite);
                    if (critSecSignal) {
                        collLock.reset();
                        autoDb.reset();
                        LOGV2(22056, "waiting till out of critical section");
                        critSecSignal->waitFor(opCtx, Seconds(10));
                    }

                    errmsg = str::stream() << "shard global version for collection is higher "
                                           << "than trying to set to '" << nss.ns() << "'";
                    result.append("ns", nss.ns());
                    requestedVersion.appendLegacyWithField(&result, "version");
                    collectionShardVersion.appendLegacyWithField(&result, "globalVersion");
                    result.appendBool("reloadConfig", true);
                    return false;
                }

                if (!collectionShardVersion.isSet() && !authoritative) {
                    // Needed b/c when the last chunk is moved off a shard, the version gets reset
                    // to zero, which should require a reload.
                    auto critSecSignal = csr->getCriticalSectionSignal(
                        opCtx, ShardingMigrationCriticalSection::kWrite);
                    if (critSecSignal) {
                        collLock.reset();
                        autoDb.reset();
                        LOGV2(22057, "waiting till out of critical section");
                        critSecSignal->waitFor(opCtx, Seconds(10));
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

        // Step 6

        // Note: The forceRefresh flag controls whether we make sure to do our
        // own refresh or if we're okay with joining another thread
        const auto status = onShardVersionMismatchNoExcept(
            opCtx, nss, requestedVersion, forceRefresh /*forceRefreshFromThisThread*/);

        {
            // Avoid using AutoGetCollection() as it returns the InvalidViewDefinition error code
            // if an invalid view is in the 'system.views' collection.
            AutoGetDb autoDb(opCtx, nss.db(), MODE_IS);
            Lock::CollectionLock collLock(opCtx, nss, MODE_IS);

            const ChunkVersion currVersion = [&] {
                auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
                auto optMetadata = csr->getCurrentMetadataIfKnown();
                return (optMetadata && optMetadata->isSharded()) ? optMetadata->getShardVersion()
                                                                 : ChunkVersion::UNSHARDED();
            }();

            if (!status.isOK()) {
                // The reload itself was interrupted or confused here
                LOGV2_WARNING(
                    22058,
                    "Could not refresh metadata for the namespace {namespace} with the requested "
                    "shard version {requestedShardVersion}; the current shard version is "
                    "{currentShardVersion}: {error}",
                    "Could not refresh metadata",
                    "namespace"_attr = nss.ns(),
                    "requestedShardVersion"_attr = requestedVersion,
                    "currentShardVersion"_attr = currVersion,
                    "error"_attr = redact(status));

                result.append("ns", nss.ns());
                result.append("code", status.code());
                requestedVersion.appendLegacyWithField(&result, "version");
                currVersion.appendLegacyWithField(&result, "globalVersion");
                result.appendBool("reloadConfig", true);

                return false;
            } else if (!requestedVersion.isWriteCompatibleWith(currVersion)) {
                // We reloaded a version that doesn't match the version mongos was trying to
                // set.
                static Occasionally sampler;
                if (sampler.tick()) {
                    LOGV2_WARNING(
                        22059,
                        "Requested shard version differs from the authoritative (current) shard "
                        "version for the namespace {namespace}; the requested version is "
                        "{requestedShardVersion}, but the current version is "
                        "{currentShardVersion}",
                        "Requested shard version differs from the authoritative (current) shard "
                        "version for this namespace",
                        "namespace"_attr = nss.ns(),
                        "requestedShardVersion"_attr = requestedVersion,
                        "currentShardVersion"_attr = currVersion);
                }

                // WARNING: the exact fields below are important for compatibility with mongos
                // version reload.

                result.append("ns", nss.ns());
                currVersion.appendLegacyWithField(&result, "globalVersion");

                // If this was a reset of a collection or the last chunk moved out, inform mongos to
                // do a full reload.
                if (currVersion.epoch() != requestedVersion.epoch() || !currVersion.isSet()) {
                    result.appendBool("reloadConfig", true);
                    // Zero-version also needed to trigger full mongos reload, sadly
                    // TODO: Make this saner, and less impactful (full reload on last chunk is bad)
                    ChunkVersion(0, 0, OID()).appendLegacyWithField(&result, "version");
                    // For debugging
                    requestedVersion.appendLegacyWithField(&result, "origVersion");
                } else {
                    requestedVersion.appendLegacyWithField(&result, "version");
                }

                return false;
            }
        }

        return true;
    }

} setShardVersionCmd;

}  // namespace
}  // namespace mongo
