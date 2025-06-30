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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <set>
#include <string>

namespace mongo {

class ChangeStreamReaderBuilder {
public:
    virtual ~ChangeStreamReaderBuilder() = default;

    /**
     * Builds a shard targeter that makes decisions about what shards to open the change stream
     * cursors on, what shards to close the cursors on when responding to control events or when
     * opening the change stream.
     */
    virtual std::unique_ptr<ChangeStreamShardTargeter> buildShardTargeter(
        OperationContext* opCtx, const ChangeStream& changeStream) = 0;

    /**
     * Builds a match expression that selects oplog entries on a data shard that correspond to
     * control events needed for proper shard targeting. The expression uses Match Expression syntax
     * and semantics.
     * The returned BSONObj instance is expected to be owned.
     */
    virtual BSONObj buildControlEventFilterForDataShard(OperationContext* opCtx,
                                                        const ChangeStream& changeStream) = 0;

    /**
     * Returns a set of names of control events on data shards used by 'ChangeStreamShardTargeter'.
     * These and only these control events will be delivered to the 'ChangeStreamShardTargeter'.
     */
    virtual std::set<std::string> getControlEventTypesOnDataShard(
        OperationContext* opCtx, const ChangeStream& changeStream) = 0;

    /**
     * Builds a match expression that selects oplog entries on the config server that correspond to
     * control events needed for proper shard targeting. The expression uses Match Expression syntax
     * and semantics.
     * The returned BSONObj instance is expected to be owned.
     */
    virtual BSONObj buildControlEventFilterForConfigServer(OperationContext* opCtx,
                                                           const ChangeStream& changeStream) = 0;

    /**
     * Returns a set of names of control events on the config server used by
     * 'ChangeStreamShardTargeter'. Otherwise the contract is the same as for
     * 'getControlEventTypesOnDataShard()'.
     */
    virtual std::set<std::string> getControlEventTypesOnConfigServer(
        OperationContext* opCtx, const ChangeStream& changeStream) = 0;

    /**
     * Service context bindings.
     */
    static ChangeStreamReaderBuilder* get(ServiceContext* serviceContext);
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<ChangeStreamReaderBuilder> builder);
};

}  // namespace mongo
