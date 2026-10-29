// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/shard_role/shard_catalog/validate_db_metadata_gen.h"
#include "mongo/util/modules.h"

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
