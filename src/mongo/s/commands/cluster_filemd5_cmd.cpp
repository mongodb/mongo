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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class FileMD5Cmd : public BasicCommand {
public:
    FileMD5Cmd() : BasicCommand("filemd5") {}

    std::string help() const override {
        return " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        std::string collectionName;
        if (const auto rootElt = cmdObj["root"]) {
            uassert(ErrorCodes::InvalidNamespace,
                    "'root' must be of type String",
                    rootElt.type() == BSONType::String);
            collectionName = rootElt.str();
        }
        if (collectionName.empty())
            collectionName = "fs";
        collectionName += ".chunks";
        return NamespaceStringUtil::parseNamespaceFromRequest(dbName, collectionName);
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::find)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        const auto cri =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        const auto callShardFn = [&](const BSONObj& cmdObj, const BSONObj& routingQuery) {
            auto shardResults =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           nss.db(),
                                                           nss,
                                                           cri,
                                                           cmdObj,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           routingQuery,
                                                           CollationSpec::kSimpleSpec,
                                                           boost::none /*letParameters*/,
                                                           boost::none /*runtimeConstants*/);
            invariant(shardResults.size() == 1);
            const auto shardResponse = uassertStatusOK(std::move(shardResults[0].swResponse));
            uassertStatusOK(shardResponse.status);

            const auto& res = shardResponse.data;
            uassertStatusOK(getStatusFromCommandResult(res));

            return res;
        };

        // If the collection is not sharded, or is sharded only on the 'files_id' field, we only
        // need to target a single shard, because the files' chunks can only be contained in a
        // single sharded chunk
        const auto& cm = cri.cm;
        if (!cm.isSharded() ||
            SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() ==
                                                        BSON("files_id" << 1))) {
            CommandHelpers::filterCommandReplyForPassthrough(
                callShardFn(
                    applyReadWriteConcern(
                        opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                    BSON("files_id" << cmdObj.firstElement())),
                &result);
            return true;
        }

        // Since the filemd5 command is tailored specifically for GridFS, there is no need to
        // support arbitrary shard keys
        uassert(ErrorCodes::IllegalOperation,
                "The GridFS fs.chunks collection must be sharded on either {files_id:1} or "
                "{files_id:1, n:1}",
                SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() ==
                                                            BSON("files_id" << 1 << "n" << 1)));

        // Theory of operation:
        //
        // Starting with n=0, send filemd5 command to shard with that chunk (gridfs chunk not
        // sharding chunk). That shard will then compute a partial md5 state (passed in the
        // "md5state" field) for all contiguous chunks that it contains. When it runs out or hits a
        // discontinuity (eg [1,2,7]) it returns what it has done so far. This is repeated as long
        // as we keep getting more chunks. The end condition is when we go to look for chunk n and
        // it doesn't exist. This means that the file's last chunk is n-1, so we return the computed
        // md5 results.

        int numGridFSChunksProcessed = 0;
        BSONObj lastResult;

        while (true) {
            const auto res = callShardFn(
                [&] {
                    BSONObjBuilder bb(applyReadWriteConcern(
                        opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)));
                    bb.append("partialOk", true);
                    bb.append("startAt", numGridFSChunksProcessed);
                    if (!lastResult.isEmpty()) {
                        bb.append(lastResult["md5state"]);
                    }
                    return bb.obj();
                }(),
                BSON("files_id" << cmdObj.firstElement() << "n" << numGridFSChunksProcessed));

            uassert(16246,
                    str::stream() << "Shard for database " << nss.db()
                                  << " is too old to support GridFS sharded by {files_id:1, n:1}",
                    res.hasField("md5state"));

            lastResult = res;

            const int numChunks = res["numChunks"].numberInt();

            if (numGridFSChunksProcessed == numChunks) {
                // No new data means we've reached the end of the file
                CommandHelpers::filterCommandReplyForPassthrough(res, &result);
                return true;
            }

            uassert(ErrorCodes::InternalError,
                    str::stream() << "Command returned numChunks of " << numChunks
                                  << " which is less than the number of chunks processes so far "
                                  << numGridFSChunksProcessed,
                    numChunks > numGridFSChunksProcessed);
            numGridFSChunksProcessed = numChunks;
        }

        MONGO_UNREACHABLE;
    }

} fileMD5Cmd;

}  // namespace
}  // namespace mongo
