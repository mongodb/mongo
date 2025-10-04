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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

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
