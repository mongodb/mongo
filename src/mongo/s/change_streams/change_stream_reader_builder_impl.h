/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/util/modules.h"

namespace mongo {

class MONGO_MOD_NEEDS_REPLACEMENT ChangeStreamReaderBuilderImpl : public ChangeStreamReaderBuilder {
public:
    /**
     * Builds CollectionChangeStreamShardTargeterImpl, DatabaseChangeStreamShardTargeterImpl or
     * AllDatabasesChangeStreamShardTargeterImpl depending on the type of the 'changeStream'.
     */
    std::unique_ptr<ChangeStreamShardTargeter> buildShardTargeter(
        OperationContext* opCtx, const ChangeStream& changeStream) override;

    /**
     * Builds a match expression that selects oplog entries on a data shard that correspond to the
     * following events:
     * - moveChunk
     * - movePrimary
     * - namespacePlacementChanged
     */
    BSONObj buildControlEventFilterForDataShard(OperationContext* opCtx,
                                                const ChangeStream& changeStream) override;

    /**
     * Returns control event types that correspond to the following events:
     * - moveChunk
     * - movePrimary
     * - namespacePlacementChanged
     */
    std::set<std::string> getControlEventTypesOnDataShard(
        OperationContext* opCtx, const ChangeStream& changeStream) override;

    /**
     * Builds a match expression that selects oplog entries on the config server that correspond to
     * dabatase creation.
     */
    BSONObj buildControlEventFilterForConfigServer(OperationContext* opCtx,
                                                   const ChangeStream& changeStream) override;

    /**
     * Returns control event type that corresponds to DatabaseCreated event.
     */
    std::set<std::string> getControlEventTypesOnConfigServer(
        OperationContext* opCtx, const ChangeStream& changeStream) override;
};

}  // namespace mongo
