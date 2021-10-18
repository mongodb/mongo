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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/balance_chunk_request_type.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

using std::string;
using str::stream;

class ConfigSvrMoveChunkCommand : public BasicCommand {
public:
    ConfigSvrMoveChunkCommand() : BasicCommand("_configsvrMoveChunk") {}

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Requests the balancer to move or rebalance a single chunk.";
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

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& unusedDbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrMoveChunk can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        opCtx->setAlwaysInterruptAtStepDownOrUp();

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto request = uassertStatusOK(
            BalanceChunkRequest::parseFromConfigCommand(cmdObj, false /* requireUUID */));

        const auto& nss = request.getNss();

        // In case of mixed binaries including v5.0, the collection UUID field may not be attached
        // to the chunk.
        if (!request.getChunk().hasCollectionUUID_UNSAFE()) {
            // TODO (SERVER-60792): Remove the following logic after v6.0 branches out.
            const auto& collection = Grid::get(opCtx)->catalogClient()->getCollection(
                opCtx, nss, repl::ReadConcernLevel::kLocalReadConcern);
            request.setCollectionUUID(collection.getUuid());  // Set collection UUID on chunk member
        }

        if (request.hasToShardId()) {
            uassertStatusOK(Balancer::get(opCtx)->moveSingleChunk(opCtx,
                                                                  nss,
                                                                  request.getChunk(),
                                                                  request.getToShardId(),
                                                                  request.getMaxChunkSizeBytes(),
                                                                  request.getSecondaryThrottle(),
                                                                  request.getWaitForDelete(),
                                                                  request.getForceJumbo()));
        } else {
            uassertStatusOK(
                Balancer::get(opCtx)->rebalanceSingleChunk(opCtx, nss, request.getChunk()));
        }

        return true;
    }

} configSvrMoveChunk;

}  // namespace
}  // namespace mongo
