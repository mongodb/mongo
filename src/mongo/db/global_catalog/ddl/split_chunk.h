// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;

class ChunkRange;
class NamespaceString;
class OperationContext;
class ScopedSplitMergeChunk;

/**
 * Attempts to split a chunk with the specified parameters. If the split fails, then the StatusWith
 * object returned will contain a Status with an ErrorCode regarding the cause of failure. If the
 * split succeeds, then the StatusWith object returned will contain Status::Ok().
 * Will update the shard's filtering metadata.
 *
 * The caller must hold the ActiveMigrationsRegistry split/merge lock for this chunk range and pass
 * it as scopedSplitMergeChunk to prove exclusive ownership. The lock must remain live for the
 * duration of the call.
 */
Status splitChunk_nonAuth(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& keyPatternObj,
                          const ChunkRange& chunkRange,
                          std::vector<BSONObj>&& splitPoints,
                          const std::string& shardName,
                          const OID& expectedCollectionEpoch,
                          const boost::optional<Timestamp>& expectedCollectionTimestamp,
                          const ScopedSplitMergeChunk& scopedSplitMergeChunk);

}  // namespace mongo
