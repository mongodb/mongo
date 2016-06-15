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

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class FsyncCommand : public Command {
public:
    FsyncCommand() : Command("fsync", false, "fsync") {}

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
        help << "invoke fsync on all shards belonging to the cluster";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::fsync);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        if (cmdObj["lock"].trueValue()) {
            errmsg = "can't do lock through mongos";
            return false;
        }

        BSONObjBuilder sub;

        bool ok = true;
        int numFiles = 0;

        std::vector<ShardId> shardIds;
        grid.shardRegistry()->getAllShardIds(&shardIds);

        for (const ShardId& shardId : shardIds) {
            const auto s = grid.shardRegistry()->getShard(txn, shardId);
            if (!s) {
                continue;
            }

            auto response =
                uassertStatusOK(s->runCommand(txn,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              "admin",
                                              BSON("fsync" << 1),
                                              Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(response.commandStatus);
            BSONObj x = std::move(response.response);

            sub.append(s->getId().toString(), x);

            if (!x["ok"].trueValue()) {
                ok = false;
                errmsg = x["errmsg"].String();
            }

            numFiles += x["numFiles"].numberInt();
        }

        result.append("numFiles", numFiles);
        result.append("all", sub.obj());
        return ok;
    }

} clusterFsyncCmd;

}  // namespace
}  // namespace mongo
