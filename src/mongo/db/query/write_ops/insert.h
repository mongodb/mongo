// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class OperationContext;

/**
 * Validates that 'doc' is legal for insertion, possibly with some modifications.
 *
 * This function returns:
 *  - a non-OK status if 'doc' is not valid;
 *  - an empty BSONObj if 'doc' can be inserted as-is; or
 *  - a non-empty BSONObj representing what should be inserted instead of 'doc'.
 *
 * If the inserted doc has any top-level $-prefixed field name , 'containsDotsOrDollarsField' is set
 * to true.
 */
StatusWith<BSONObj> fixDocumentForInsert(OperationContext* opCtx,
                                         const BSONObj& doc,
                                         bool bypassEmptyTsReplacement = false,
                                         bool* containsDotsOrDollarsField = nullptr);

/**
 * Returns Status::OK() if this namespace is valid for user write operations.  If not, returns
 * an error Status.
 */
Status userAllowedWriteNS(OperationContext* opCtx, const NamespaceString& ns);

/**
 * Checks if the namespace is valid for user create operations.
 */
Status userAllowedCreateNS(OperationContext* opCtx, const NamespaceString& ns);
}  // namespace mongo
