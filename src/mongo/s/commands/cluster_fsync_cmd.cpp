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

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class FsyncCommand : public ErrmsgCommandDeprecated {
public:
    FsyncCommand() : ErrmsgCommandDeprecated("fsync") {}

    std::string help() const override {
        return "invoke fsync on all shards belonging to the cluster";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
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
        actions.addAction(ActionType::fsync);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        if (cmdObj["lock"].trueValue()) {
            errmsg = "can't do lock through mongos";
            return false;
        }

        BSONObjBuilder sub;

        bool ok = true;

        auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto shardIds = shardRegistry->getAllShardIds(opCtx);

        for (const ShardId& shardId : shardIds) {
            auto shardStatus = shardRegistry->getShard(opCtx, shardId);
            if (!shardStatus.isOK()) {
                continue;
            }
            const auto s = std::move(shardStatus.getValue());

            auto response = uassertStatusOK(s->runCommandWithFixedRetryAttempts(
                opCtx,
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
        }

        // This field has had dummy value since MMAP went away. It is undocumented.
        // Maintaining it so as not to cause unnecessary user pain across upgrades.
        result.append("numFiles", 1);
        result.append("all", sub.obj());

        return ok;
    }

} clusterFsyncCmd;

}  // namespace
}  // namespace mongo
