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

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"

#include <utility>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Indicates if and how the change event fetching can proceed.
 */
enum class ShardTargeterDecision {
    // Fetching of change stream events can continue.
    kContinue,

    // The change stream reader should switch to V1 of the change stream reader.
    kSwitchToV1,
};

class ChangeStreamShardTargeter {
public:
    virtual ~ChangeStreamShardTargeter() = default;

    /**
     * Enacts opening of the initial set of cursors on shards/config server when opening of a change
     * stream is triggered.
     * The change stream is opened at 'atClusterTime'.
     * Preconditions:
     * - None of the other methods have been invoked before.
     * - The change stream is read in Strict mode.
     * Postconditions:
     * - Requests to open cursors through context at a time >= 'atClusterTime' are made (may skip
     *   scanning of irrelevant oplog segments by using cluster time greater than 'atClusterTime' if
     *   shard placement history indicates that it is correct to do so).
     * - Returns value 'ShardTargeterDecision::kSwitchToV1' if precise information about
     *   collection/database allocation to shards is not available for the given change stream open
     *   cluster time.
     */
    virtual ShardTargeterDecision initialize(OperationContext* opCtx,
                                             Timestamp atClusterTime,
                                             ChangeStreamReaderContext& context) = 0;

    /**
     * Enacts opening/closing of cursors on data shards and/or config server when starting fetching
     * of a new change stream event sequence segment (later called change stream segment).
     * Determines the bounds of the change stream segment based on what data shards are available at
     * the invocation time and the shard placement history.
     * The change stream is read in Ignore-Removed-Shards mode.
     *
     * Returns value 'ShardTargeterDecision::kSwitchToV1' (the first member of the returned pair) if
     * precise information about collection/database allocation to shards is not available for the
     * given cluster time.
     *
     * The second member of the returned pair is:
     * - timestamp 'segmentEndTime' which defines the end point (exclusive) of the bounded change
     *   stream segment and indicates that at least one of the shards on which the collection/
     *   database has been allocated to at time 'segmentStartTime' has been removed.
     *   'segmentEndTime' is equal to the earliest collection/database allocation to a shard set
     *   change effective time while segmentStartTime < segmentEndTime. This implicitly indicates
     *   that the reading of the change stream segment should happen in degraded mode.
     * - absent, when none of the shards on which the collection/database has been allocated to at a
     *   time 'segmentStartTime' has been removed. This indicates that the change stream segment is
     *   unbounded and the change stream must be read in Normal mode.
     */
    virtual std::pair<ShardTargeterDecision, boost::optional<Timestamp>> startChangeStreamSegment(
        OperationContext* opCtx, Timestamp atClusterTime, ChangeStreamReaderContext& context) = 0;

    /**
     * Processes and reacts to a control change event.
     * Preconditions:
     * - 'initialize()' to have been invoked if the change stream is read in Strict mode.
     * - 'startChangeStreamSegment()' to have been invoked if in Ignore-Removed-Shards mode.
     * Postconditions:
     * - In Normal mode requests to open and/or close cursors are made. The targeter should assume
     *   that opening/closing of cursors will happen after this method returns.
     * - In Degraded mode requests to open and/or close cursors cannot be made - the set of tracked
     *   shards for a bounded change stream segment is fixed.
     */
    virtual ShardTargeterDecision handleEvent(OperationContext* opCtx,
                                              const Document& event,
                                              ChangeStreamReaderContext& context) = 0;
};

}  // namespace mongo
