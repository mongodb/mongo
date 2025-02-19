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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/future_util.h"
#include <future>

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/util/cancellation.h"

namespace mongo {

/**
 * Responsible for monitoring select change streams events locally on shard during during the
 * resharding application.
 */
class ReshardingChangeStreamsMonitor
    : public std::enable_shared_from_this<ReshardingChangeStreamsMonitor> {
public:
    using BatchProcessedCallback =
        std::function<void(int documentsDelta, BSONObj resumeToken, bool completed)>;

    ReshardingChangeStreamsMonitor(NamespaceString monitorNamespace,
                                   Timestamp startAtOperationTime,
                                   bool isRecipient,
                                   BatchProcessedCallback callback);

    ReshardingChangeStreamsMonitor(NamespaceString monitorNamespace,
                                   BSONObj startAfterToken,
                                   bool isRecipient,
                                   BatchProcessedCallback callback);

    /**
     * Schedules work to open a local change streams and track the events.
     */
    void startMonitoring(std::shared_ptr<executor::TaskExecutor> executor,
                         CancelableOperationContextFactory factory);

    /**
     * Blocks on a future that becomes ready when either:
     *   (a) final change event has been reached, or
     *   (b) stepdown or abort.
     */
    SharedSemiFuture<void> awaitFinalChangeEvent();

private:
    /**
     * Creates the aggregation command request for the change streams.
     */
    AggregateCommandRequest _makeAggregateCommandRequest();

    /**
     * Continuously fetch and process events from the change streams.
     */
    void _consumeChangeEvents(OperationContext* opCtx, const AggregateCommandRequest& aggRequest);
    void _processChangeEvent(BSONObj changeEvent);

    const NamespaceString _monitorNS;
    const boost::optional<Timestamp> _startAt;
    const boost::optional<BSONObj> _startAfter;
    const bool _isRecipient;
    const BatchProcessedCallback _batchProcessedCallback;

    // Records the change in documents as the events are observed.
    int _documentsDelta = 0;

    // Records the number of events processed.
    int _numEventsProcessed = 0;

    bool _receivedFinalEvent = false;
    std::unique_ptr<SharedPromise<void>> _finalEventPromise;
};

}  // namespace mongo
