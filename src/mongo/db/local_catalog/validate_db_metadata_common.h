/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/local_catalog/validate_db_metadata_gen.h"

namespace mongo {

struct ValidateDBMetadataSizeTracker {
    bool incrementAndCheckOverflow(const ErrorReplyElement& obj) {
        // The field name in the array should be at most 7 digits. In addition each element will use
        // 2 additional bytes for type byte, and null termination of the field name. Note that we
        // are intentionally over estmating the size the delta here, so that we have sufficient
        // space for other fields in the output.
        currentSize += obj.toBSON().objsize() + 15;
        return currentSize < BSONObjMaxUserSize;
    }

private:
    size_t currentSize = 0;
};

inline void assertUserCanRunValidate(OperationContext* opCtx,
                                     const ValidateDBMetadataCommandRequest& request) {
    const auto tenantId = request.getDbName().tenantId();
    const auto resource = request.getDb()
        ? ResourcePattern::forDatabaseName(DatabaseNameUtil::deserialize(
              tenantId, *request.getDb(), request.getSerializationContext()))
        : ResourcePattern::forAnyNormalResource(tenantId);
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Not authorized to run validateDBMetadata command on resource: '"
                          << resource.toString() << "'",
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(resource, ActionType::validate));
}
}  // namespace mongo
