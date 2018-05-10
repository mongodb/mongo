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
#include "mongo/db/commands/kill_op_cmd_base.h"
#include "mongo/rpc/metadata.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

class ClusterKillOpCommand : public KillOpCmdBase {
public:
    bool run(OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        BSONElement element = cmdObj.getField("op");
        uassert(50759, "Did not provide \"op\" field", element.ok());

        if (isKillingLocalOp(element)) {
            const unsigned int opId = KillOpCmdBase::parseOpId(cmdObj);
            killLocalOperation(opCtx, opId);

            // killOp always reports success once past the auth check.
            return true;
        } else if (element.type() == BSONType::String) {
            // It's a string. Should be of the form shardid:opid.
            return _killShardOperation(opCtx, element.str(), result);
        }

        uasserted(50760,
                  str::stream() << "\"op\" field was of unsupported type " << element.type());
    }

private:
    static bool _killShardOperation(OperationContext* opCtx,
                                    const std::string& opToKill,
                                    BSONObjBuilder& result) {
        // The format of op is shardid:opid
        // This is different than the format passed to the mongod killOp command.
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
        log() << "want to kill op: " << redact(opToKill);

        // Will throw if shard id is not found
        auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardIdent);
        uassertStatusOK(shardStatus.getStatus());
        auto shard = shardStatus.getValue();

        int opId;
        uassertStatusOK(parseNumberFromStringWithBase(opToKill.substr(opSepPos + 1), 10, &opId));

        // shardid is actually the opid - keeping for backwards compatibility.
        result.append("shard", shardIdent);
        result.append("shardid", opId);

        ScopedDbConnection conn(shard->getConnString());
        // intentionally ignore return value - that is how legacy killOp worked.
        conn->runCommand(OpMsgRequest::fromDBAndBody("admin", BSON("killOp" << 1 << "op" << opId)));
        conn.done();

        // The original behavior of killOp on mongos is to always return success, regardless of
        // whether the shard reported success or not.
        return true;
    }

} clusterKillOpCommand;

}  // namespace
}  // namespace mongo
