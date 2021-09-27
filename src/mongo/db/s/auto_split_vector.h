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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Given a chunk, determines whether it satisfies the requisites to be auto-splitted and - if so -
 * returns the split points (shard keys representing the lower bounds of the new chunks to create).
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
 */
std::vector<BSONObj> autoSplitVector(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& keyPattern,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     long long maxChunkSizeBytes);

/*
 * Utility function for deserializing autoSplitVector/splitVector responses.
 */
static std::vector<BSONObj> parseSplitKeys(const BSONElement& splitKeysArray) {
    uassert(ErrorCodes::TypeMismatch,
            "The split keys vector must be represented as a BSON array",
            !splitKeysArray.eoo() && splitKeysArray.type() == BSONType::Array);

    std::vector<BSONObj> splitKeys;
    for (const auto& elem : splitKeysArray.Obj()) {
        uassert(ErrorCodes::TypeMismatch,
                "Each element of the split keys array must be an object",
                elem.type() == BSONType::Object);
        splitKeys.push_back(elem.embeddedObject().getOwned());
    }

    return splitKeys;
}

}  // namespace mongo
