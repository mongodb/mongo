// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
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
 * Append to the given builder the formatted 'explain' response that describes the work done in the
 * two phase write protocol.
 **/
void generateExplainResponseForTwoPhaseWriteProtocol(
    BSONObjBuilder& explainOutputBuilder,
    const BSONObj& clusterQueryWithoutShardKeyExplainObj,
    const BSONObj& clusterWriteWithoutShardKeyExplainObj);

}  // namespace write_without_shard_key
}  // namespace mongo
