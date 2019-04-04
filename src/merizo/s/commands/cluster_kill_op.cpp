/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kCommand

#include "merizo/platform/basic.h"

#include <string>

#include "merizo/bson/bsonelement.h"
#include "merizo/bson/bsonobj.h"
#include "merizo/bson/bsonobjbuilder.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/client/connpool.h"
#include "merizo/db/audit.h"
#include "merizo/db/auth/authorization_session.h"
#include "merizo/db/commands.h"
#include "merizo/db/commands/kill_op_cmd_base.h"
#include "merizo/rpc/metadata.h"
#include "merizo/s/client/shard.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/grid.h"
#include "merizo/util/log.h"
#include "merizo/util/merizoutils/str.h"

namespace merizo {
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
        // This is different than the format passed to the merizod killOp command.
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

        // The original behavior of killOp on merizos is to always return success, regardless of
        // whether the shard reported success or not.
        return true;
    }

} clusterKillOpCommand;

}  // namespace
}  // namespace merizo
