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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/start_chunk_clone_request.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class RecvChunkStartCommand : public ErrmsgCommandDeprecated {
public:
    RecvChunkStartCommand() : ErrmsgCommandDeprecated("_recvChunkStart") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "internal";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        // This is required to be true to support moveChunk.
        return true;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceString(dbName.tenantId(), CommandHelpers::parseNsFullyQualified(cmdObj));
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool shouldCheckoutSession() const final {
        return false;
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

        auto nss = NamespaceString(parseNs({boost::none, dbname}, cmdObj));

        auto cloneRequest = uassertStatusOK(StartChunkCloneRequest::createFromCommand(nss, cmdObj));

        const auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));

        const auto writeConcern =
            uassertStatusOK(ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(
                opCtx, cloneRequest.getSecondaryThrottle()));

        // Ensure this shard is not currently receiving or donating any chunks.
        auto scopedReceiveChunk(
            uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerReceiveChunk(
                opCtx,
                nss,
                chunkRange,
                cloneRequest.getFromShardId(),
                false /* waitForCompletionOfConflictingOps*/)));

        // We force a refresh immediately after registering this migration to guarantee that this
        // shard will not receive a chunk after refreshing.
        onShardVersionMismatch(opCtx, nss, boost::none);
        const auto shardId = ShardingState::get(opCtx)->shardId();

        const auto collectionEpoch = [&] {
            AutoGetCollection autoColl(opCtx, nss, MODE_IS);
            auto const optMetadata =
                CollectionShardingRuntime::get(opCtx, nss)->getCurrentMetadataIfKnown();
            uassert(StaleConfigInfo(nss,
                                    ShardVersion::IGNORED() /* receivedVersion */,
                                    boost::none /* wantedVersion */,
                                    shardId,
                                    boost::none),
                    "The collection's sharding state was cleared by a concurrent operation",
                    optMetadata);
            return optMetadata->getShardVersion().epoch();
        }();

        uassertStatusOK(
            MigrationDestinationManager::get(opCtx)->start(opCtx,
                                                           nss,
                                                           std::move(scopedReceiveChunk),
                                                           cloneRequest,
                                                           collectionEpoch,
                                                           writeConcern));

        result.appendBool("started", true);
        return true;
    }

} recvChunkStartCmd;

class RecvChunkStatusCommand : public BasicCommand {
public:
    RecvChunkStatusCommand() : BasicCommand("_recvChunkStatus") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "internal";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        bool waitForSteadyOrDone = cmdObj["waitForSteadyOrDone"].boolean();
        MigrationDestinationManager::get(opCtx)->report(result, opCtx, waitForSteadyOrDone);
        return true;
    }

} recvChunkStatusCommand;

class RecvChunkCommitCommand : public BasicCommand {
public:
    RecvChunkCommitCommand() : BasicCommand("_recvChunkCommit") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "internal";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }


    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const sessionId = uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj));
        auto const mdm = MigrationDestinationManager::get(opCtx);

        Status const status = mdm->startCommit(sessionId);
        mdm->report(result, opCtx, false);
        if (!status.isOK()) {
            LOGV2(22014,
                  "_recvChunkCommit failed: {error}",
                  "_recvChunkCommit failed",
                  "error"_attr = redact(status));
            uassertStatusOK(status);
        }
        return true;
    }

} recvChunkCommitCommand;

class RecvChunkAbortCommand : public BasicCommand {
public:
    RecvChunkAbortCommand() : BasicCommand("_recvChunkAbort") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "internal";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const mdm = MigrationDestinationManager::get(opCtx);

        auto migrationSessionIdStatus(MigrationSessionId::extractFromBSON(cmdObj));

        if (migrationSessionIdStatus.isOK()) {
            Status const status = mdm->abort(migrationSessionIdStatus.getValue());
            mdm->report(result, opCtx, false);
            if (!status.isOK()) {
                LOGV2(22015,
                      "_recvChunkAbort failed: {error}",
                      "_recvChunkAbort failed",
                      "error"_attr = redact(status));
                uassertStatusOK(status);
            }
        } else if (migrationSessionIdStatus == ErrorCodes::NoSuchKey) {
            mdm->abortWithoutSessionIdCheck();
            mdm->report(result, opCtx, false);
        }

        uassertStatusOK(migrationSessionIdStatus.getStatus());
        return true;
    }

} recvChunkAbortCommand;

class RecvChunkReleaseCritSecCommand : public BasicCommand {
public:
    RecvChunkReleaseCritSecCommand() : BasicCommand("_recvChunkReleaseCritSec") {}

    std::string help() const override {
        return "internal";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }


    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());
        const auto sessionId = uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj));

        LOGV2_DEBUG(5899101, 2, "Received _recvChunkReleaseCritSec", "sessionId"_attr = sessionId);

        const auto mdm = MigrationDestinationManager::get(opCtx);
        const auto status = mdm->exitCriticalSection(opCtx, sessionId);
        if (!status.isOK()) {
            LOGV2(5899109,
                  "_recvChunkReleaseCritSec failed: {error}",
                  "_recvChunkReleaseCritSec failed",
                  "error"_attr = redact(status));
            uassertStatusOK(status);
        }
        return true;
    }

} recvChunkReleaseCritSecCommand;

}  // namespace
}  // namespace mongo
