// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/kill_op_cmd_base.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

class KillOpCommand : public KillOpCmdBase {
public:
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        auto opIds = KillOpCmdBase::parseOpIds(cmdObj);
        ErrorCodes::Error errorCode = KillOpCmdBase::parseErrorCode(opCtx, cmdObj);

        // Used by tests to check if auth checks passed.
        result.append("info", "attempting to kill op");
        for (auto opId : opIds) {
            LOGV2(20482, "Going to kill op", "opId"_attr = opId);
            KillOpCmdBase::killLocalOperation(opCtx, opId, errorCode);
        }
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
