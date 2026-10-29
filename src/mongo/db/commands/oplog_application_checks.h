// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

namespace mongo {

/** `uassert` that `elem` type matches the specified `type`. */
void checkBSONType(BSONType type, const BSONElement& elem);

// OplogApplicationValidity represents special conditions relevant to authorization for
// oplog application.
//
// kNeedsSuperuser means the oplog application command is empty or contains an empty nested
// applyOps command oplog entry, or a createCollection or renameCollection mixed in a batch.
//
// kNeedsUseUUID means any oplog entry in the command contains a UUID, so the useUUID action
// must be authorized.
//
// kNeedsForceAndUseUUID means the command contains one oplog entry which is a collection create
// with a specified UUID, so both the forceUUID and useUUID actions must be authorized.
//
// kOk means no special conditions apply.
enum class OplogApplicationValidity { kOk, kNeedsUseUUID, kNeedsForceAndUseUUID, kNeedsSuperuser };

// OplogApplicationChecks contains helper functions for checking the applyOps command.
class OplogApplicationChecks {
public:
    /**
     * Checks the authorization for an entire oplog application command.
     */
    static Status checkAuthForOperation(OperationContext* opCtx,
                                        const DatabaseName& dbName,
                                        const BSONObj& cmdObj,
                                        OplogApplicationValidity validity);

    /**
     * Checks that 'opsElement' is an array and all elements of the array are valid operations.
     */
    static Status checkOperationArray(const BSONElement& opsElement);

private:
    static UUID getUUIDFromOplogEntry(const BSONObj& oplogEntry);

    /**
     * Checks the authorization for a single operation contained within an oplog application
     * command.
     */
    static Status checkOperationAuthorization(OperationContext* opCtx,
                                              const DatabaseName& dbName,
                                              const BSONObj& oplogEntry,
                                              AuthorizationSession* authSession);
    /**
     * Returns OK if 'e' contains a valid operation.
     */
    static Status checkOperation(const BSONElement& e);
};

}  // namespace mongo
