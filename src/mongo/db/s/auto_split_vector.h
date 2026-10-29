// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Given a chunk, determines whether it satisfies the requisites to be auto-splitted and - if so -
 * returns the split points (shard keys representing the lower bounds of the new chunks to create)
 * along with a bool representing whether or not the max bson size for the vector of split points
 * was reached. If the bool returns true, this function should be called on the same range again to
 * retrieve the rest of the split points.
 *
 * The logic implemented can be summarized as follows: given a `maxChunkSize` of `x` MB, the
 * algorithm aims to choose the split points so that the resulting chunks' size would be around
 * `maxChunkSize`. As it is too expensive to precisely determine the dimension of a chunk, it is
 * assumed a uniform distribution of document sizes, hence the aim is to balance the number of
 * documents per chunk.
 *
 * ======= ALGORITHM DESCRIPTION =======
 *
 * The split points for a chunk `C` belonging to a collection `coll` are calculated as follows:
 * - `averageDocumentSize` = `totalCollSizeOnShard / numberOfCollDocs`
 * - `maxNumberOfDocsPerChunk` = `maxChunkSize / averageDocumentSize`
 * - Scan forward the shard key index entries for `coll` that are belonging to chunk `C`:
 * - (1) Choose a split point every `maxNumberOfDocsPerChunk` scanned keys.
 * - (2) As it needs to be avoided the creation of small chunks, consider the number of documents
 * `S` that the right-most chunk would contain given the calculated split points:
 * --- (2.1) IF `S >= 80% maxNumberOfDocsPerChunk`, return the list of calculated split points.
 * --- (2.2) ELSE IF `S` documents could be fairly redistributed in the last chunks so that their
 * size would be at least `67% maxNumberOfDocsPerChunk`: recalculate the last split points (max 3).
 * --- (2.3) ELSE simply remove the last split point and keep a bigger last chunk.
 *
 *
 * ============== EXAMPLES =============
 *
 * ========= EXAMPLE (CASE 2.1) ========
 * `maxChunkSize` = 100MB
 * `averageDocumentSize` = 1MB
 * `maxNumberOfDocsPerChunk` = 100
 *
 * Shard key type: integer
 * Chunk `C` bounds: `[0, maxKey)` . Chunk `C` contains 190 documents with shard keys [0...189].
 *
 * (1) Initially calculated split points: [99].
 * (2) The last chunk would contain the interval `[99-189]` so `S = 90`
 * (2.1) `S >= 80% maxChunkSize`, so keep the current split points.
 *
 * Returned split points: [99].
 * Returned continuation flag: false.
 *
 * ========= EXAMPLE (CASE 2.2) ========
 * `maxChunkSize` = 100MB
 * `averageDocumentSize` = 1MB
 * `maxNumberOfDocsPerChunk` = 100
 *
 * Shard key type: integer
 * Chunk `C` bounds: `[0, maxKey)` . Chunk `C` contains 140 documents with shard keys [0...139].
 *
 * (1) Initially calculated split points: [99].
 * (2) The last chunk would contain the interval `[99-139]` so `S = 40`
 * (2.2) `S` documents can be redistributed on the last split point by generating chunks of size >=
 * 67% maxChunkSize. Recalculate.
 *
 * Returned split points: [69].
 * Returned continuation flag: false.
 *
 * ========= EXAMPLE (CASE 2.3) ========
 * `maxChunkSize` = 100MB
 * `averageDocumentSize` = 1MB
 * `maxNumberOfDocsPerChunk` = 100
 *
 * Shard key type: integer
 * Chunk `C` bounds: `[0, maxKey)` . Chunk `C` contains 120 documents with shard keys [0...119].
 *
 * (1) Initially calculated split points: [99].
 * (2) The last chunk would contain the interval `[99-119]` so `S = 20`
 * (2.3) `S` documents can't be redistributed on the last split point by generating chunks of size
 * >= 67% maxChunkSize. So remove the last split point.
 *
 * Returned split points: [].
 * Returned continuation flag: false.
 */
std::pair<std::vector<BSONObj>, bool> autoSplitVector(OperationContext* opCtx,
                                                      const CollectionAcquisition& acquisition,
                                                      const BSONObj& keyPattern,
                                                      const BSONObj& min,
                                                      const BSONObj& max,
                                                      long long maxChunkSizeBytes,
                                                      boost::optional<int> limit = boost::none,
                                                      bool forward = true);

/*
 * Utility function for deserializing autoSplitVector/splitVector responses.
 */
static std::vector<BSONObj> parseSplitKeys(const BSONElement& splitKeysArray) {
    uassert(ErrorCodes::TypeMismatch,
            "The split keys vector must be represented as a BSON array",
            !splitKeysArray.eoo() && splitKeysArray.type() == BSONType::array);

    std::vector<BSONObj> splitKeys;
    for (const auto& elem : splitKeysArray.Obj()) {
        uassert(ErrorCodes::TypeMismatch,
                "Each element of the split keys array must be an object",
                elem.type() == BSONType::object);
        splitKeys.push_back(elem.embeddedObject().getOwned());
    }

    return splitKeys;
}

}  // namespace mongo
