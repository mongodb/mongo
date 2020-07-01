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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <set>
#include <string>

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/util/debug_util.h"

namespace mongo {
namespace {

// WARNING: This command is deprecated and will be removed in v4.6. (TODO SERVER-47817)
class CmdShardingResetErrorDeprecated : public BasicCommand {
public:
    CmdShardingResetErrorDeprecated() : BasicCommand("resetError", "reseterror") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        // No auth required
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        if (_sampler.tick()) {
            LOGV2_WARNING(4718706, "The resetError command is deprecated.");
        }

        LastError::get(cc()).reset();

        const std::set<std::string>* shards = ClusterLastErrorInfo::get(cc())->getPrevShardHosts();

        for (std::set<std::string>::const_iterator i = shards->begin(); i != shards->end(); i++) {
            const std::string shardName = *i;

            ShardConnection conn(opCtx, ConnectionString(shardName, ConnectionString::SET), "");

            BSONObj res;

            // Don't care about result from shards.
            conn->runCommand(
                dbname,
                applyReadWriteConcern(
                    opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                res);
            conn.done();
        }

        return true;
    }

private:
    // Used to log occasional deprecation warnings when this command is invoked.
    Rarely _sampler;
} cmdShardingResetErrorDeprecated;

}  // namespace
}  // namespace mongo
