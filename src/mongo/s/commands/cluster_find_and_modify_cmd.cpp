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

#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/find_and_modify.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

using std::shared_ptr;
using std::string;
using std::vector;

class FindAndModifyCmd : public Command {
public:
    FindAndModifyCmd() : Command("findAndModify", false, "findandmodify") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    virtual Status explain(OperationContext* txn,
                           const std::string& dbName,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                           BSONObjBuilder* out) const {
        const NamespaceString nss = parseNsCollectionRequired(dbName, cmdObj);

        auto scopedDB = uassertStatusOK(ScopedShardDatabase::getExisting(txn, dbName));
        DBConfig* conf = scopedDB.db();

        shared_ptr<ChunkManager> chunkMgr;
        shared_ptr<Shard> shard;

        if (!conf->isShardingEnabled() || !conf->isSharded(nss.ns())) {
            shard = Grid::get(txn)->shardRegistry()->getShard(txn, conf->getPrimaryId());
        } else {
            chunkMgr = _getChunkManager(txn, conf, nss);

            const BSONObj query = cmdObj.getObjectField("query");

            StatusWith<BSONObj> status = _getShardKey(txn, chunkMgr, query);
            if (!status.isOK()) {
                return status.getStatus();
            }

            BSONObj shardKey = status.getValue();
            shared_ptr<Chunk> chunk = chunkMgr->findIntersectingChunk(txn, shardKey);

            shard = Grid::get(txn)->shardRegistry()->getShard(txn, chunk->getShardId());
        }

        BSONObjBuilder explainCmd;
        int options = 0;
        ClusterExplain::wrapAsExplain(
            cmdObj, verbosity, serverSelectionMetadata, &explainCmd, &options);

        // Time how long it takes to run the explain command on the shard.
        Timer timer;

        BSONObjBuilder result;
        bool ok = _runCommand(txn, conf, chunkMgr, shard->getId(), nss, explainCmd.obj(), result);
        long long millisElapsed = timer.millis();

        if (!ok) {
            BSONObj res = result.obj();
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Explain for findAndModify failed: " << res);
        }

        Strategy::CommandResult cmdResult;
        cmdResult.shardTargetId = shard->getId();
        cmdResult.target = shard->getConnString();
        cmdResult.result = result.obj();

        vector<Strategy::CommandResult> shardResults;
        shardResults.push_back(cmdResult);

        return ClusterExplain::buildExplainResult(
            txn, shardResults, ClusterExplain::kSingleShard, millisElapsed, out);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbName,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nss = parseNsCollectionRequired(dbName, cmdObj);

        // findAndModify should only be creating database if upsert is true, but this would require
        // that the parsing be pulled into this function.
        auto scopedDb = uassertStatusOK(ScopedShardDatabase::getOrCreate(txn, dbName));
        DBConfig* conf = scopedDb.db();

        if (!conf->isShardingEnabled() || !conf->isSharded(nss.ns())) {
            return _runCommand(txn, conf, nullptr, conf->getPrimaryId(), nss, cmdObj, result);
        }

        shared_ptr<ChunkManager> chunkMgr = _getChunkManager(txn, conf, nss);

        const BSONObj query = cmdObj.getObjectField("query");

        StatusWith<BSONObj> status = _getShardKey(txn, chunkMgr, query);
        if (!status.isOK()) {
            // Bad query
            return appendCommandStatus(result, status.getStatus());
        }

        BSONObj shardKey = status.getValue();
        shared_ptr<Chunk> chunk = chunkMgr->findIntersectingChunk(txn, shardKey);

        bool ok = _runCommand(txn, conf, chunkMgr, chunk->getShardId(), nss, cmdObj, result);
        if (ok) {
            // check whether split is necessary (using update object for size heuristic)
            chunk->splitIfShould(txn, cmdObj.getObjectField("update").objsize());
        }

        return ok;
    }

private:
    shared_ptr<ChunkManager> _getChunkManager(OperationContext* txn,
                                              DBConfig* conf,
                                              const NamespaceString& nss) const {
        shared_ptr<ChunkManager> chunkMgr = conf->getChunkManager(txn, nss.ns());
        massert(13002, "shard internal error chunk manager should never be null", chunkMgr);

        return chunkMgr;
    }

    StatusWith<BSONObj> _getShardKey(OperationContext* txn,
                                     shared_ptr<ChunkManager> chunkMgr,
                                     const BSONObj& query) const {
        // Verify that the query has an equality predicate using the shard key
        StatusWith<BSONObj> status =
            chunkMgr->getShardKeyPattern().extractShardKeyFromQuery(txn, query);

        if (!status.isOK()) {
            return status;
        }

        BSONObj shardKey = status.getValue();

        if (shardKey.isEmpty()) {
            return Status(ErrorCodes::ShardKeyNotFound,
                          "query for sharded findAndModify must have shardkey");
        }

        return shardKey;
    }

    bool _runCommand(OperationContext* txn,
                     DBConfig* conf,
                     shared_ptr<ChunkManager> chunkManager,
                     const ShardId& shardId,
                     const NamespaceString& nss,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) const {
        BSONObj res;

        const auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, shardId);
        ShardConnection conn(shard->getConnString(), nss.ns(), chunkManager);
        bool ok = conn->runCommand(conf->name(), cmdObj, res);
        conn.done();

        // ErrorCodes::RecvStaleConfig is the code for RecvStaleConfigException.
        if (!ok && res.getIntField("code") == ErrorCodes::RecvStaleConfig) {
            // Command code traps this exception and re-runs
            throw RecvStaleConfigException("FindAndModify", res);
        }

        // First append the properly constructed writeConcernError. It will then be skipped
        // in appendElementsUnique.
        if (auto wcErrorElem = res["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, result);
        }

        result.appendElementsUnique(res);
        return ok;
    }

} findAndModifyCmd;

}  // namespace
}  // namespace mongo
