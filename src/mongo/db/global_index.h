/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/util/uuid.h"

namespace mongo::global_index {

/**
 * Creates the internal collection implements the global index container with the given UUID on the
 * shard. Replicates as a 'createGlobalIndex' command. This container-backing collection:
 * - is clustered by _id. The _id field stores the cluster-unique document key of the index entry;
 * - has a secondary unique index on the 'ik' field which stores a binary-comparable
 *   representation of the index key value.
 */
void createContainer(OperationContext* opCtx, const UUID& indexUUID);

/**
 * Inserts a key into the global index container identified by UUID. Replicates as an 'xi' command.
 * - 'key' is the unique index key.
 * - 'docKey' is the document key of the index entry.
 */
void insertKey(OperationContext* opCtx,
               const UUID& indexUUID,
               const BSONObj& key,
               const BSONObj& docKey);

}  // namespace mongo::global_index
