/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/find_and_modify_common.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/cluster_write.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

BSONObj getCollation(const BSONObj& cmdObj) {
    BSONElement collationElement;
    auto status = bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
    if (status.isOK()) {
        return collationElement.Obj();
    } else if (status != ErrorCodes::NoSuchKey) {
        uassertStatusOK(status);
    }

    return BSONObj();
}

BSONObj getShardKey(OperationContext* opCtx, const ChunkManager& chunkMgr, const BSONObj& query) {
    BSONObj shardKey =
        uassertStatusOK(chunkMgr.getShardKeyPattern().extractShardKeyFromQuery(opCtx, query));
    uassert(ErrorCodes::ShardKeyNotFound,
            "Query for sharded findAndModify must contain the shard key",
            !shardKey.isEmpty());
    return shardKey;
}

class FindAndModifyCmd : public BasicCommand {
public:
    FindAndModifyCmd() : BasicCommand("findAndModify", "findandmodify") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   ExplainOptions::Verbosity verbosity,
                   BSONObjBuilder* out) const override {
        const NamespaceString nss(parseNsCollectionRequired(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        std::shared_ptr<ChunkManager> chunkMgr;
        std::shared_ptr<Shard> shard;

        if (!routingInfo.cm()) {
            shard = routingInfo.primary();
        } else {
            chunkMgr = routingInfo.cm();

            const BSONObj query = cmdObj.getObjectField("query");
            const BSONObj collation = getCollation(cmdObj);
            const BSONObj shardKey = getShardKey(opCtx, *chunkMgr, query);
            const auto chunk = chunkMgr->findIntersectingChunk(shardKey, collation);

            shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk->getShardId()));
        }

        const auto explainCmd = ClusterExplain::wrapAsExplain(cmdObj, verbosity);

        // Time how long it takes to run the explain command on the shard.
        Timer timer;
        BSONObjBuilder result;
        _runCommand(opCtx,
                    shard->getId(),
                    (chunkMgr ? chunkMgr->getVersion(shard->getId()) : ChunkVersion::UNSHARDED()),
                    nss,
                    explainCmd,
                    &result);
        const auto millisElapsed = timer.millis();

        Strategy::CommandResult cmdResult;
        cmdResult.shardTargetId = shard->getId();
        cmdResult.target = shard->getConnString();
        cmdResult.result = result.obj();

        std::vector<Strategy::CommandResult> shardResults;
        shardResults.push_back(cmdResult);

        return ClusterExplain::buildExplainResult(
            opCtx, shardResults, ClusterExplain::kSingleShard, millisElapsed, out);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNsCollectionRequired(dbName, cmdObj));

        // findAndModify should only be creating database if upsert is true, but this would require
        // that the parsing be pulled into this function.
        uassertStatusOK(createShardDatabase(opCtx, nss.db()));

        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            _runCommand(
                opCtx, routingInfo.primaryId(), ChunkVersion::UNSHARDED(), nss, cmdObj, &result);
            return true;
        }

        const auto chunkMgr = routingInfo.cm();

        const BSONObj query = cmdObj.getObjectField("query");
        const BSONObj collation = getCollation(cmdObj);
        const BSONObj shardKey = getShardKey(opCtx, *chunkMgr, query);
        auto chunk = chunkMgr->findIntersectingChunk(shardKey, collation);

        _runCommand(opCtx,
                    chunk->getShardId(),
                    chunkMgr->getVersion(chunk->getShardId()),
                    nss,
                    cmdObj,
                    &result);
        updateChunkWriteStatsAndSplitIfNeeded(
            opCtx, chunkMgr.get(), chunk.get(), cmdObj.getObjectField("update").objsize());

        return true;
    }

private:
    static void _runCommand(OperationContext* opCtx,
                            const ShardId& shardId,
                            const ChunkVersion& shardVersion,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            BSONObjBuilder* result) {
        const auto response = [&] {
            std::vector<AsyncRequestsSender::Request> requests;
            requests.emplace_back(
                shardId,
                appendShardVersion(filterCommandRequestForPassthrough(cmdObj), shardVersion));

            AsyncRequestsSender ars(opCtx,
                                    Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                    nss.db().toString(),
                                    requests,
                                    kPrimaryOnlyReadPreference,
                                    opCtx->getTxnNumber() ? Shard::RetryPolicy::kIdempotent
                                                          : Shard::RetryPolicy::kNoRetry);

            auto response = ars.next();
            invariant(ars.done());

            return uassertStatusOK(std::move(response.swResponse));
        }();

        uassertStatusOK(response.status);

        const auto responseStatus = getStatusFromCommandResult(response.data);
        if (ErrorCodes::isStaleShardingError(responseStatus.code())) {
            // Command code traps this exception and re-runs
            throw StaleConfigException("findAndModify", response.data);
        }

        // First append the properly constructed writeConcernError. It will then be skipped in
        // appendElementsUnique.
        if (auto wcErrorElem = response.data["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
        }

        result->appendElementsUnique(filterCommandReplyForPassthrough(response.data));
    }

} findAndModifyCmd;

}  // namespace
}  // namespace mongo
