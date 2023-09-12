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


#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/kill_op_cmd_base.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

class KillOpCommand : public KillOpCmdBase {
public:
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        long long opId = KillOpCmdBase::parseOpId(cmdObj);

        // Used by tests to check if auth checks passed.
        result.append("info", "attempting to kill op");
        LOGV2(20482, "Going to kill op: {opId}", "Going to kill op", "opId"_attr = opId);
        KillOpCmdBase::killLocalOperation(opCtx, opId);
        reportSuccessfulCompletion(opCtx, dbName, cmdObj);

        // killOp always reports success once past the auth check.
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(KillOpCommand).forShard();

}  // namespace mongo
