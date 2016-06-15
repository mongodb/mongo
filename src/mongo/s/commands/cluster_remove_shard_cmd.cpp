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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

class RemoveShardCmd : public Command {
public:
    RemoveShardCmd() : Command("removeShard", false, "removeshard") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "remove a shard from the system.";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::removeShard);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const string target = cmdObj.firstElement().valuestrsafe();

        const auto s = grid.shardRegistry()->getShard(txn, ShardId(target));
        if (!s) {
            string msg(str::stream() << "Could not drop shard '" << target
                                     << "' because it does not exist");
            log() << msg;
            return appendCommandStatus(result, Status(ErrorCodes::ShardNotFound, msg));
        }

        auto catalogClient = grid.catalogClient(txn);
        StatusWith<ShardDrainingStatus> removeShardResult =
            catalogClient->removeShard(txn, s->getId());
        if (!removeShardResult.isOK()) {
            return appendCommandStatus(result, removeShardResult.getStatus());
        }

        vector<string> databases;
        Status status = catalogClient->getDatabasesForShard(txn, s->getId(), &databases);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        // Get BSONObj containing:
        // 1) note about moving or dropping databases in a shard
        // 2) list of databases (excluding 'local' database) that need to be moved
        BSONObj dbInfo;
        {
            BSONObjBuilder dbInfoBuilder;
            dbInfoBuilder.append("note", "you need to drop or movePrimary these databases");
            BSONArrayBuilder dbs(dbInfoBuilder.subarrayStart("dbsToMove"));
            for (vector<string>::const_iterator it = databases.begin(); it != databases.end();
                 it++) {
                if (*it != "local") {
                    dbs.append(*it);
                }
            }
            dbs.doneFast();
            dbInfo = dbInfoBuilder.obj();
        }

        // TODO: Standardize/Seperate how we append to the result object
        switch (removeShardResult.getValue()) {
            case ShardDrainingStatus::STARTED:
                result.append("msg", "draining started successfully");
                result.append("state", "started");
                result.append("shard", s->getId().toString());
                result.appendElements(dbInfo);
                break;
            case ShardDrainingStatus::ONGOING: {
                vector<ChunkType> chunks;
                Status status =
                    catalogClient->getChunks(txn,
                                             BSON(ChunkType::shard(s->getId().toString())),
                                             BSONObj(),
                                             boost::none,  // return all
                                             &chunks,
                                             nullptr);
                if (!status.isOK()) {
                    return appendCommandStatus(result, status);
                }

                result.append("msg", "draining ongoing");
                result.append("state", "ongoing");
                {
                    BSONObjBuilder inner;
                    inner.append("chunks", static_cast<long long>(chunks.size()));
                    inner.append("dbs", static_cast<long long>(databases.size()));
                    BSONObj b = inner.obj();
                    result.append("remaining", b);
                }
                result.appendElements(dbInfo);
                break;
            }
            case ShardDrainingStatus::COMPLETED:
                result.append("msg", "removeshard completed successfully");
                result.append("state", "completed");
                result.append("shard", s->getId().toString());
        }

        return true;
    }

} removeShardCmd;

}  // namespace
}  // namespace mongo
