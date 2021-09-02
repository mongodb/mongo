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
 * The logic implemented can be summarized as follows: given a maxChunkSize of `x` MB, the algorithm
 * aims to choose the split points so that the resulting chunks' size would be around `x / 2` MB.
 * As it is too expensive to precisely determine the dimension of a chunk, it is assumed a uniform
 * distribution of document sizes, hence the aim is to balance the number of documents per chunk.
 *
 * ALGORITHM DESCRIPTION
 *
 * The split points for a chunk `C` belonging to a collection `coll` are calculated as follows:
 * - `averageDocumentSize` = `totalCollSizeOnShard / numberOfCollDocs`
 * - `maxNumberOfDocsPerChunk` = `maxChunkSize / averageDocumentSize`
 * - `maxNumberOfDocsPerSplittedChunk` = `maxNumberOfDocsPerChunk / 2`
 * - Scan forward the shard key index entries for `coll` that are belonging to chunk `C`:
 * - (1) Choose a split point every `maxNumberOfDocsPerSplittedChunk` scanned keys
 * - (2) To avoid small chunks, remove the last split point and eventually recalculate it as
 * follows:
 * --- (2.1) IF the right-most interval's number of document is at least 90%
 * `maxNumberOfDocsPerChunk`, pick the middle key.
 * --- (2.2) ELSE no last split point (wait for more inserts and next iterations of the splitter).
 *
 * EXAMPLE
 * `maxChunkSize` = 100MB
 * `averageDocumentSize` = 1MB
 * `maxNumberOfDocsPerChunk` = 100
 * `maxNumberOfDocsPerSplittedChunk` = 50
 *
 * Shard key type: integer
 * Chunk `C` bounds: `[0, maxKey)` . Chunk `C` contains 190 documents with shard keys [0...189].
 *
 * (1) Initially calculated split points: [49, 99, 149].
 * (2) Removing the last split point `149` to avoid small chunks and recalculate:
 * (2.1) Is the interval `[99-189]` eligible to be split? YES, because it contains 90 documents
 * that is equivalent to 90% `maxNumberOfDocsPerChunk`: choose the middle key `134` as split point .
 *
 * Returned split points: [49, 99, 134].
 */
std::vector<BSONObj> autoSplitVector(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& keyPattern,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     long long maxChunkSizeBytes);

}  // namespace mongo
