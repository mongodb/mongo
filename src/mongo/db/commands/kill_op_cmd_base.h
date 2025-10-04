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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"

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
    /**
     * Kill an operation running on this instance of mongod or mongos.
     */
    static void killLocalOperation(OperationContext* opCtx, OperationId opToKill);

    /**
     * Extract the "op" field from 'cmdObj' and convert the value to unsigned int. Since BSON only
     * supports signed number types, and an opId is unsigned, the "op" field of 'cmdObj' may be
     * negative, so that it can be stored in a signed type. The conversion back to unsigned is
     * taken care of here.
     */
    static unsigned int parseOpId(const BSONObj& cmdObj);

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
