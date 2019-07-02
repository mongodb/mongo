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
#include <string>

#include "mongo/base/status.h"
#include "mongo/util/uuid.h"

namespace mongo {
class BSONElement;
class BSONObj;
class OperationContext;

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
    static Status checkAuthForCommand(OperationContext* opCtx,
                                      const std::string& dbname,
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
                                              const std::string& dbname,
                                              const BSONObj& oplogEntry,
                                              AuthorizationSession* authSession,
                                              bool alwaysUpsert);
    /**
     * Returns OK if 'e' contains a valid operation.
     */
    static Status checkOperation(const BSONElement& e);
};

}  // namespace mongo
