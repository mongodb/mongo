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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
class WriteConcernErrorDetail;

namespace write_without_shard_key {

// Used as a "dummy" target document for constructing explain responses for single writes without
// shard key.
const BSONObj targetDocForExplain = BSON("_id" << "WriteWithoutShardKey");

/**
 * Uses updateDriver to produce the document to insert. Only use when {upsert: true}.
 */
std::pair<BSONObj, BSONObj> generateUpsertDocument(
    OperationContext* opCtx,
    const UpdateRequest& updateRequest,
    const UUID& collectionUUID,
    boost::optional<TimeseriesOptions> timeseriesOptions,
    const StringDataComparator* comparator);

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
                         const boost::optional<LegacyRuntimeConstants>& legacyRuntimeConstants,
                         bool isTimeseriesViewRequest);

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
 * The write phase can produce a 'WriteConcernError', which can be orthogonal to other errors
 * reported by the write. The optional 'wce' out variable can be used to capture the
 * 'WriteConcernError' separately, so the caller can handle it.
 **/
StatusWith<ClusterWriteWithoutShardKeyResponse> runTwoPhaseWriteProtocol(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    boost::optional<WriteConcernErrorDetail>& wce);
/**
 * Return a formatted 'explain' response that describes the work done in the two phase write
 * protocol.
 **/
BSONObj generateExplainResponseForTwoPhaseWriteProtocol(
    const BSONObj& clusterQueryWithoutShardKeyExplainObj,
    const BSONObj& clusterWriteWithoutShardKeyExplainObj);

}  // namespace write_without_shard_key
}  // namespace mongo
