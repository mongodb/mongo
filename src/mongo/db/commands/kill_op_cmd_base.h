// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"

#include <vector>

namespace mongo {

/**
 * Base class for the killOp command, which attempts to kill a given operation. Contains code
 * common to mongos and mongod implementations.
 */
class KillOpCmdBase : public BasicCommand {
public:
    KillOpCmdBase() : BasicCommand("killOp") {}

    ~KillOpCmdBase() override = default;

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const final;

protected:
    static constexpr ErrorCodes::Error kDefaultErrorCode = ErrorCodes::Interrupted;

    /**
     * Kill an operation running on this instance of mongod or mongos.
     */
    static void killLocalOperation(OperationContext* opCtx,
                                   OperationId opToKill,
                                   ErrorCodes::Error killCode);

    /**
     * Extract the "op" field from 'cmdObj' and convert the value to unsigned int. Since BSON only
     * supports signed number types, and an opId is unsigned, the "op" field of 'cmdObj' may be
     * negative, so that it can be stored in a signed type. The conversion back to unsigned is
     * taken care of here.
     */
    static unsigned int parseOpId(const BSONObj& cmdObj);

    /**
     * Extract the "op" field from 'cmdObj' and return a list of opIDs to kill. Accepts either a
     * single opID or an array of opIDs.
     */
    static std::vector<unsigned int> parseOpIds(const BSONObj& cmdObj);

    /**
     * Extract the "errorCode" field from 'cmdObj' and convert the value to ErrorCodes::Error. If
     * the field is missing, will return the default kill code for the command.
     */
    static ErrorCodes::Error parseErrorCode(OperationContext* opCtx, const BSONObj& cmdObj);

    static void reportSuccessfulCompletion(OperationContext* opCtx,
                                           const DatabaseName& dbName,
                                           const BSONObj& cmdObj);

    /**
     * Return whether the operation being killed is "local" or not. All operations on a mongod are
     * local. On a mongos, killOp may may kill an operation on a shard, or an operation "local" to
     * the mongos.
     *
     * Expects to be passed the "op" field of the command object.
     */
    static bool isKillingLocalOp(const BSONElement& opElem);
};

}  // namespace mongo
