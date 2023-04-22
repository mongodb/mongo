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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"

namespace mongo {
namespace write_without_shard_key {

/**
 * Uses updateDriver to produce the document to insert. Only use when {upsert: true}.
 */
BSONObj generateUpsertDocument(OperationContext* opCtx, const UpdateRequest& updateRequest);

/**
 * Returns true if we can use the two phase protocol to complete a single write without shard
 * key.
 **/
bool useTwoPhaseProtocol(OperationContext* opCtx,
                         NamespaceString ns,
                         bool isUpdateOrDelete,
                         bool isUpsert,
                         const BSONObj& query,
                         const BSONObj& collation,
                         const boost::optional<BSONObj>& let,
                         const boost::optional<LegacyRuntimeConstants>& legacyRuntimeConstants);

/**
 * Runs and returns the result of running a write without a shard key using the two phase protocol.
 * At a high level, the two phase protocol involves two phases:
 *
 * 1. Read Phase:
 * Using the query from the original write request, we broadcast the query to all of the shards (a
 * subset of shards if we have a partial shard key) and select one of the shards that has a matching
 * document and designate that shard as the executor of the write.
 *
 * 2. Write Phase:
 * Using the information about the shard chosen in the first phase, send the write directly to the
 * shard to execute.
 *
 * Both phases are run transactionally using an internal transaction.
 *
 **/
StatusWith<ClusterWriteWithoutShardKeyResponse> runTwoPhaseWriteProtocol(OperationContext* opCtx,
                                                                         NamespaceString nss,
                                                                         BSONObj cmdObj);

}  // namespace write_without_shard_key
}  // namespace mongo
