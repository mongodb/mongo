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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

class ClusterKillOpCommand : public Command {
public:
    ClusterKillOpCommand() : Command("killOp") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const final {
        return true;
    }

    bool adminOnly() const final {
        return true;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::killop);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        // The format of op is shardid:opid
        // This is different than the format passed to the mongod killOp command.
        std::string opToKill;
        uassertStatusOK(bsonExtractStringField(cmdObj, "op", &opToKill));

        const auto opSepPos = opToKill.find(':');

        uassert(28625,
                str::stream() << "The op argument to killOp must be of the format shardid:opid"
                              << " but found \""
                              << opToKill
                              << '"',
                (opToKill.size() >= 3) &&                  // must have at least N:N
                    (opSepPos != std::string::npos) &&     // must have ':' as separator
                    (opSepPos != 0) &&                     // can't be :NN
                    (opSepPos != (opToKill.size() - 1)));  // can't be NN:

        auto shardIdent = opToKill.substr(0, opSepPos);
        log() << "want to kill op: " << opToKill;

        // Will throw if shard id is not found
        auto shard = grid.shardRegistry()->getShard(txn, shardIdent);
        if (!shard) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::ShardNotFound,
                       str::stream() << "shard " << shardIdent << " does not exist"));
        }

        auto opId = std::stoi(opToKill.substr(opSepPos + 1));

        // shardid is actually the opid - keeping for backwards compatibility.
        result.append("shard", shardIdent);
        result.append("shardid", opId);

        ScopedDbConnection conn(shard->getConnString());
        // intentionally ignore return value - that is how legacy killOp worked.
        conn->runCommandWithMetadata(
            "admin", "killOp", rpc::makeEmptyMetadata(), BSON("killOp" << 1 << "op" << opId));
        conn.done();

        // The original behavior of killOp on mongos is to always return success, regardless of
        // whether the shard reported success or not.
        return true;
    }

} clusterKillOpCommand;

}  // namespace
}  // namespace mongo
