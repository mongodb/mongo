// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future_util.h"
#include "mongo/util/modules.h"

#include <future>

namespace mongo {

/**
 * Responsible for monitoring select change streams events locally on shard during during the
 * resharding application.
 */
class ReshardingChangeStreamsMonitor
    : public std::enable_shared_from_this<ReshardingChangeStreamsMonitor> {
public:
    enum Role { kDonor, kRecipient };

    class EventBatch {
    public:
        EventBatch(Role role);

        /**
         * Adds the event to the batch.
         */
        void add(const BSONObj& event);

        /**
         * Returns true if this monitor should stop adding more events to this batch, i.e. if one of
         * the following is true.
         * - The cursor has reached the final event.
         * - The batch has reached the configured size limit.
         * - The batch has reached the configured time limit.
         */
        bool shouldDispose();

        /**
         * Sets the resume token after this batch. This is the 'postBatchResumeToken' for the
         * $changeStream aggregate or latest getMore command.
         */
        void setResumeToken(BSONObj resumeToken);

        /**
         * Returns true if this batch contains the final event, i.e. it is the final batch.
         */
        bool containsFinalEvent() const;

        /**
         * Returns true if the batch is empty.
         */
        bool empty() const;

        /**
         * Getters.
         */
        int64_t getNumEvents() const;
        int64_t getDocumentsDelta() const;
        BSONObj getResumeToken() const;

        /**
         * Returns the clusterTime (seconds component) from the postBatchResumeToken, or
         * boost::none if the resume token has not been set yet.
         */
        boost::optional<uint32_t> getResumeTokenClusterTimeSecs() const;

    private:
        const Role _role;
        // The timestamp at which the batch started.
        const Date_t _createdAt;

        bool _containsFinalEvent = false;
        // The resume token after this batch,
        BSONObj _resumeToken;
        // The number of events in this batch.
        int64_t _numEvents = 0;
        // The change in documents based on the events in this batch.
        int64_t _documentsDelta = 0;
    };

    using BatchProcessedCallback = std::function<void(const EventBatch& batch)>;

    ReshardingChangeStreamsMonitor(UUID reshardingUUID,
                                   NamespaceString monitorNss,
                                   Timestamp startAtOperationTime,
                                   boost::optional<BSONObj> startAfterResumeToken,
                                   BatchProcessedCallback callback);

    virtual ~ReshardingChangeStreamsMonitor() = default;

    /**
     * Schedules work to open a local change streams and track the events.
     */
    SemiFuture<void> startMonitoring(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    /**
     * Waits until the monitor has consumed the final change event or the 'executor' has been
     * shut down or the cancellation source for 'factory' has been cancelled.
     */
    SharedSemiFuture<void> awaitFinalChangeEvent();

    /**
     * Waits until the monitor has cleaned up the change stream cursor or the 'cleanupExecutor'
     * has been shut down. This is the last step in the monitor.
     */
    SharedSemiFuture<void> awaitCleanup();

    /**
     * Used for testing only. Returns the number of events and batches consumed, respectively. Can
     * only be called after the monitor is completed since there is no locking to prevent concurrent
     * access to the variables.
     */
    int64_t numEventsTotalForTest();
    int64_t numBatchesForTest();

    /**
     * Returns the 'comment' for the $changeStream aggregate command that is unique for the given
     * resharding UUID. The 'comment' is used to identify the cursors to kill when the monitor
     * completes.
     */
    BSONObj makeAggregateComment(const UUID& reshardingUUID);

    /**
     * Creates the aggregation command request for the change streams monitor.
     */
    AggregateCommandRequest makeAggregateCommandRequest();

protected:
    /**
     * If there are open change stream cursors with the resharding UUID, kills them and returns
     * the status.
     */
    virtual Status killCursors(OperationContext* opCtx);

private:
    /**
     * Helper to construct an opCtx and set non-deprioritizable state. Since this class exists
     * both outside of and within the critical section but has no concept of the resharding phases,
     * it is always non-deprioritizable.
     */
    CancelableOperationContext _makeOperationContext(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const;

    /**
     * Creates the aggregation pipeline for the change streams monitor.
     */
    std::vector<BSONObj> _makeAggregatePipeline() const;

    /**
     * If the monitor has already opened a change stream cursor, creates a DBClientCursor from the
     * existing cursor id. Otherwise, creates a DBClientCursor by opening a change stream cursor
     * through running the change stream aggregate command, and then stores the cursor id.
     * Returns the resulting DBClientCursor.
     */
    std::unique_ptr<mongo::DBClientCursor> _makeDBClientCursor(DBDirectClient* client);

    /**
     * Continuously fetches and processes events from the change streams until the monitor has
     * consumed the final change event or the 'executor' has been shut down or the cancellation
     * source for 'factory' has been cancelled.
     */
    ExecutorFuture<void> _consumeChangeEvents(
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    const UUID _reshardingUUID;
    const NamespaceString _monitorNss;
    const Timestamp _startAtOperationTime;
    const boost::optional<BSONObj> _startAfterResumeToken;
    const Role _role;
    const BatchProcessedCallback _batchProcessedCallback;

    boost::optional<CursorId> _cursorId;
    // The total number of events and batches consumed.
    int64_t _numEventsTotal = 0;
    int64_t _numBatches = 0;

    bool _receivedFinalEvent = false;
    std::unique_ptr<SharedPromise<void>> _finalEventPromise;
    std::unique_ptr<SharedPromise<void>> _cleanupPromise;
};

}  // namespace mongo
