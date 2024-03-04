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

#include <memory>
#include <string>

#include "mongo/base/parse_number.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/commands/kill_op_cmd_base.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ClusterKillOpCommand : public KillOpCmdBase {
public:
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        BSONElement element = cmdObj.getField("op");
        uassert(50759, "Did not provide \"op\" field", element.ok());

        if (isKillingLocalOp(element)) {
            const unsigned int opId = KillOpCmdBase::parseOpId(cmdObj);
            killLocalOperation(opCtx, opId);
            reportSuccessfulCompletion(opCtx, dbName, cmdObj);

            // killOp always reports success once past the auth check.
            return true;
        } else if (element.type() == BSONType::String) {
            // It's a string. Should be of the form shardid:opid.
            if (_killShardOperation(opCtx, element.str(), result)) {
                reportSuccessfulCompletion(opCtx, dbName, cmdObj);
                return true;
            } else {
                return false;
            }
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
                              << " but found \"" << opToKill << '"',
                (opToKill.size() >= 3) &&                  // must have at least N:N
                    (opSepPos != std::string::npos) &&     // must have ':' as separator
                    (opSepPos != 0) &&                     // can't be :NN
                    (opSepPos != (opToKill.size() - 1)));  // can't be NN:

        auto shardIdent = opToKill.substr(0, opSepPos);
        LOGV2(22754, "About to kill op", "opToKill"_attr = redact(opToKill));

        // Will throw if shard id is not found
        auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardIdent);
        uassertStatusOK(shardStatus.getStatus());
        auto shard = shardStatus.getValue();

        int opId;
        uassertStatusOK(NumberParser().base(10)(opToKill.substr(opSepPos + 1), &opId));

        // shardid is actually the opid - keeping for backwards compatibility.
        result.append("shard", shardIdent);
        result.append("shardid", opId);

        ScopedDbConnection conn(shard->getConnString());
        BSONObjBuilder bob(BSON("killOp" << 1 << "op" << opId));
        APIParameters::get(opCtx).appendInfo(&bob);
        // intentionally ignore return value - that is how legacy killOp worked.
        conn->runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired /* admin is not per-tenant. */,
            DatabaseName::kAdmin,
            bob.obj()));
        conn.done();

        // The original behavior of killOp on mongos is to always return success, regardless of
        // whether the shard reported success or not.
        return true;
    }
};
MONGO_REGISTER_COMMAND(ClusterKillOpCommand).forRouter();

}  // namespace
}  // namespace mongo
