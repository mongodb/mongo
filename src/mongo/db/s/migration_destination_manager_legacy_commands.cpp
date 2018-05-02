/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class RecvChunkStartCommand : public ErrmsgCommandDeprecated {
public:
    RecvChunkStartCommand() : ErrmsgCommandDeprecated("_recvChunkStart") {}

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

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        const ShardId toShard(cmdObj["toShardName"].String());
        const ShardId fromShard(cmdObj["fromShardName"].String());

        const NamespaceString nss(cmdObj.firstElement().String());

        const auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));

        const auto shardVersion = forceShardFilteringMetadataRefresh(opCtx, nss);

        // Process secondary throttle settings and assign defaults if necessary.
        const auto secondaryThrottle =
            uassertStatusOK(MigrationSecondaryThrottleOptions::createFromCommand(cmdObj));
        const auto writeConcern = uassertStatusOK(
            ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(opCtx, secondaryThrottle));

        BSONObj shardKeyPattern = cmdObj["shardKeyPattern"].Obj().getOwned();

        auto statusWithFromShardConnectionString = ConnectionString::parse(cmdObj["from"].String());
        if (!statusWithFromShardConnectionString.isOK()) {
            errmsg = str::stream()
                << "cannot start receiving chunk " << redact(chunkRange.toString())
                << causedBy(redact(statusWithFromShardConnectionString.getStatus()));

            warning() << errmsg;
            return false;
        }

        const MigrationSessionId migrationSessionId(
            uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj)));

        // Ensure this shard is not currently receiving or donating any chunks.
        auto scopedReceiveChunk(uassertStatusOK(
            ActiveMigrationsRegistry::get(opCtx).registerReceiveChunk(nss, chunkRange, fromShard)));

        uassertStatusOK(MigrationDestinationManager::get(opCtx)->start(
            nss,
            std::move(scopedReceiveChunk),
            migrationSessionId,
            statusWithFromShardConnectionString.getValue(),
            fromShard,
            toShard,
            chunkRange.getMin(),
            chunkRange.getMax(),
            shardKeyPattern,
            shardVersion.epoch(),
            writeConcern));

        result.appendBool("started", true);
        return true;
    }

} recvChunkStartCmd;

class RecvChunkStatusCommand : public BasicCommand {
public:
    RecvChunkStatusCommand() : BasicCommand("_recvChunkStatus") {}

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
             const std::string& dbname,
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
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const sessionId = uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj));
        auto const mdm = MigrationDestinationManager::get(opCtx);
        Status const status = mdm->startCommit(sessionId);
        mdm->report(result, opCtx, false);
        if (!status.isOK()) {
            log() << status.reason();
            uassertStatusOK(status);
        }
        return true;
    }

} recvChunkCommitCommand;

class RecvChunkAbortCommand : public BasicCommand {
public:
    RecvChunkAbortCommand() : BasicCommand("_recvChunkAbort") {}

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
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const mdm = MigrationDestinationManager::get(opCtx);

        auto migrationSessionIdStatus(MigrationSessionId::extractFromBSON(cmdObj));

        if (migrationSessionIdStatus.isOK()) {
            Status const status = mdm->abort(migrationSessionIdStatus.getValue());
            mdm->report(result, opCtx, false);
            if (!status.isOK()) {
                log() << status.reason();
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

}  // namespace
}  // namespace mongo
