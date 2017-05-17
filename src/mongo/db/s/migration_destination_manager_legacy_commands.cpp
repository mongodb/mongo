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

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

class RecvChunkStartCommand : public Command {
public:
    RecvChunkStartCommand() : Command("_recvChunkStart") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        // This is required to be true to support moveChunk.
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             string& errmsg,
             BSONObjBuilder& result) {
        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        const ShardId toShard(cmdObj["toShardName"].String());
        const ShardId fromShard(cmdObj["fromShardName"].String());

        const NamespaceString nss(cmdObj.firstElement().String());

        const auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));

        // Refresh our collection manager from the config server, we need a collection manager to
        // start registering pending chunks. We force the remote refresh here to make the behavior
        // consistent and predictable, generally we'd refresh anyway, and to be paranoid.
        ChunkVersion currentVersion;

        Status status = shardingState->refreshMetadataNow(opCtx, nss, &currentVersion);
        if (!status.isOK()) {
            errmsg = str::stream() << "cannot start receiving chunk "
                                   << redact(chunkRange.toString()) << causedBy(redact(status));
            warning() << errmsg;
            return false;
        }

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
        auto scopedRegisterReceiveChunk(
            uassertStatusOK(shardingState->registerReceiveChunk(nss, chunkRange, fromShard)));

        uassertStatusOK(shardingState->migrationDestinationManager()->start(
            nss,
            std::move(scopedRegisterReceiveChunk),
            migrationSessionId,
            statusWithFromShardConnectionString.getValue(),
            fromShard,
            toShard,
            chunkRange.getMin(),
            chunkRange.getMax(),
            shardKeyPattern,
            currentVersion.epoch(),
            writeConcern));

        result.appendBool("started", true);
        return true;
    }

} recvChunkStartCmd;

class RecvChunkStatusCommand : public Command {
public:
    RecvChunkStatusCommand() : Command("_recvChunkStatus") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             string& errmsg,
             BSONObjBuilder& result) {
        ShardingState::get(opCtx)->migrationDestinationManager()->report(result);
        return true;
    }

} recvChunkStatusCommand;

class RecvChunkCommitCommand : public Command {
public:
    RecvChunkCommitCommand() : Command("_recvChunkCommit") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             string& errmsg,
             BSONObjBuilder& result) {
        auto const sessionId = uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj));
        auto mdm = ShardingState::get(opCtx)->migrationDestinationManager();
        Status const status = mdm->startCommit(sessionId);
        mdm->report(result);
        if (!status.isOK()) {
            log() << status.reason();
            return appendCommandStatus(result, status);
        }
        return true;
    }

} recvChunkCommitCommand;

class RecvChunkAbortCommand : public Command {
public:
    RecvChunkAbortCommand() : Command("_recvChunkAbort") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             string& errmsg,
             BSONObjBuilder& result) {
        auto const mdm = ShardingState::get(opCtx)->migrationDestinationManager();

        auto migrationSessionIdStatus(MigrationSessionId::extractFromBSON(cmdObj));

        if (migrationSessionIdStatus.isOK()) {
            Status const status = mdm->abort(migrationSessionIdStatus.getValue());
            mdm->report(result);
            if (!status.isOK()) {
                log() << status.reason();
                return appendCommandStatus(result, status);
            }
        } else if (migrationSessionIdStatus == ErrorCodes::NoSuchKey) {
            mdm->abortWithoutSessionIdCheck();
            mdm->report(result);
        }
        uassertStatusOK(migrationSessionIdStatus.getStatus());
        return true;
    }

} recvChunkAbortCommand;

}  // namespace
}  // namespace mongo
