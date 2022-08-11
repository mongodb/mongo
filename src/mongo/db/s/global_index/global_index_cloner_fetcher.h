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

#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/shard_id.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace global_index {

class GlobalIndexClonerFetcherInterface {
public:
    struct FetchedEntry {
    public:
        FetchedEntry(BSONObj _documentKey, BSONObj _indexKeyValues)
            : documentKey(_documentKey.getOwned()), indexKeyValues(_indexKeyValues.getOwned()) {}

        BSONObj documentKey;
        BSONObj indexKeyValues;
    };

    virtual ~GlobalIndexClonerFetcherInterface() {}

    /**
     * Returns the next document to clone.
     * Returns boost::none if there are no documents left.
     */
    virtual boost::optional<FetchedEntry> getNext(OperationContext* opCtx) = 0;
};

/**
 * Responsible for fetching documents to clone for a particular shard.
 */
class GlobalIndexClonerFetcher : public GlobalIndexClonerFetcherInterface {
public:
    GlobalIndexClonerFetcher(NamespaceString nss,
                             UUID collUUId,
                             UUID indexUUID,
                             ShardId myShardId,
                             Timestamp minFetchTimestamp,
                             KeyPattern sourceShardKeyPattern,
                             KeyPattern globalIndexPattern);

    boost::optional<FetchedEntry> getNext(OperationContext* opCtx) override;

    /**
     * Builds the aggregation pipeline for fetching the documents
     */
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(OperationContext* opCtx);

private:
    std::unique_ptr<Pipeline, PipelineDeleter> _restartPipeline(OperationContext* opCtx);
    std::unique_ptr<Pipeline, PipelineDeleter> _targetAggregationRequest(const Pipeline& pipeline);
    BSONObj _buildProjectionSpec();

    const NamespaceString _nss;
    const UUID _collUUID;
    const UUID _indexUUID;
    const ShardId _myShardId;
    const Timestamp _minFetchTimestamp;
    const KeyPattern _sourceShardKeyPattern;
    const KeyPattern _globalIndexKeyPattern;

    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
};

}  // namespace global_index
}  // namespace mongo
