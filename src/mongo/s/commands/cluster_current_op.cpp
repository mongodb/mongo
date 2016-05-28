/*
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <tuple>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/commands/run_on_all_shards_cmd.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kInprogFieldName[] = "inprog";
const char kOpIdFieldName[] = "opid";
const char kClientFieldName[] = "client";
// awkward underscores used to make this visually distinct from kClientFieldName
const char kClient_S_FieldName[] = "client_s";

const char kCommandName[] = "currentOp";

class ClusterCurrentOpCommand : public RunOnAllShardsCommand {
public:
    ClusterCurrentOpCommand() : RunOnAllShardsCommand(kCommandName) {}

    bool adminOnly() const final {
        return true;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::inprog);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void aggregateResults(const std::vector<ShardAndReply>& results, BSONObjBuilder& output) final {
        // Each shard responds with a document containing an array of subdocuments.
        // Each subdocument represents an operation running on that shard.
        // We merge the responses into a single document containg an array
        // of the operations from all shards.

        // There are two modifications we make.
        // 1) we prepend the shardid (with a colon separator) to the opid of each operation.
        // This allows users to pass the value of the opid field directly to killOp.

        // 2) we change the field name of "client" to "client_s". This is because each
        // client is actually a mongos.

        // TODO: failpoint for a shard response being invalid.

        // Error handling - we maintain the same behavior as legacy currentOp/inprog
        // that is, if any shard replies with an invalid response (i.e. it does not
        // contain a field 'inprog' that is an array), we ignore it.
        //
        // If there is a lower level error (i.e. the command fails, network error, etc)
        // RunOnAllShardsCommand will handle returning an error to the user.
        BSONArrayBuilder aggregatedOpsBab(output.subarrayStart(kInprogFieldName));

        for (auto&& shardResponse : results) {
            StringData shardName;
            BSONObj shardResponseObj;
            std::tie(shardName, shardResponseObj) = shardResponse;

            auto shardOps = shardResponseObj[kInprogFieldName];

            // legacy behavior
            if (!shardOps.isABSONObj()) {
                warning() << "invalid currentOp response from shard " << shardName
                          << ", got: " << shardOps;
                continue;
            }

            for (auto&& shardOp : shardOps.Obj()) {
                BSONObjBuilder modifiedShardOpBob;

                // maintain legacy behavior
                // but log it first
                if (!shardOp.isABSONObj()) {
                    warning() << "invalid currentOp response from shard " << shardName
                              << ", got: " << shardOp;
                    continue;
                }

                for (auto&& shardOpElement : shardOp.Obj()) {
                    auto fieldName = shardOpElement.fieldNameStringData();
                    if (fieldName == kOpIdFieldName) {
                        uassert(28630,
                                str::stream() << "expected numeric opid from currentOp response"
                                              << " from shard "
                                              << shardName
                                              << ", got: "
                                              << shardOpElement,
                                shardOpElement.isNumber());

                        modifiedShardOpBob.append(kOpIdFieldName,
                                                  str::stream() << shardName << ":"
                                                                << shardOpElement.numberInt());
                    } else if (fieldName == kClientFieldName) {
                        modifiedShardOpBob.appendAs(shardOpElement, kClient_S_FieldName);
                    } else {
                        modifiedShardOpBob.append(shardOpElement);
                    }
                }
                modifiedShardOpBob.done();
                // append the modified document to the output array
                aggregatedOpsBab.append(modifiedShardOpBob.obj());
            }
        }
        aggregatedOpsBab.done();
    }

} clusterCurrentOpCmd;

}  // namespace
}  // namespace mongo
