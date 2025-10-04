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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/query/exec/merge_cursors_stage.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/time_support.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * An internal stage used as part of the V2 change streams infrastructure, to listen for events that
 * describe topology changes to the cluster. This stage is responsible for opening and closing
 * remote cursors on these shards as needed.
 */
class ChangeStreamHandleTopologyChangeV2Stage final : public Stage {
public:
    /**
     * Possible states of the internal state machine used inside doGetNext and its callees.
     */
    enum class State {
        // Start state. Will always transition to another state.
        kUninitialized,

        // Waiting state: waiting for future cluster time to arrive. Can be entered multiple times.
        kWaiting,

        // Fetching initialization. Will only be entered once during the lifetime of the stage and
        // will always transition to another state.
        kFetchingInitialization,

        // Get the individual change events. Can be entered repeatedly. Only reachable in strict
        // mode.
        kFetchingGettingChangeEvent,

        // Start a change stream segment. Can be entered repeatedly. Only reachable in
        // ignoreRemovedShards mode.
        kFetchingStartingChangeStreamSegment,

        // Get the individual change events in normal mode. Can be entered repeatedly. Only
        // reachable in ignoreRemovedShards mode.
        kFetchingNormalGettingChangeEvent,

        // Get the individual change events in degraded mode. Can be entered repeatedly. Only
        // reachable in ignoreRemovedShards mode.
        kFetchingDegradedGettingChangeEvent,

        // Downgrade change stream reader to v1. This is a terminal state.
        kDowngrading,

        // Some error has happened. This is a terminal state.
        kFinal,
    };

    /**
     * Generic interface for waiting until a certain deadline has been reached.
     */
    class DeadlineWaiter {
    public:
        virtual ~DeadlineWaiter() = default;

        /**
         * Wait until the specified deadline has been reached, the OperationContext's deadline has
         * expired, the OperationContext has been killed/interrupted or another error occurs.
         *
         * Behavior:
         * - if the deadline has been reached, but the OperationContext's own deadline has not been
         * exceeded, simply returns.
         * - if the OperationContext's deadline has been exceeded, throws a timelimit exceeded
         * exception whose error code can vary based on how the OperationContext is configured.
         * - in case the OperationContext as interrupted, marked as killed or the corresponding
         * client has disconnected, throws a DBException that can have several different error
         * codes, e.g. 'ClientMarkedKilled', 'InterruptedAtShutdown', the specific error code used
         * when the client disconnects.
         */
        virtual void waitUntil(OperationContext* opCtx, Date_t deadline) = 0;
    };

    /**
     * An interface used to open and close cursors.
     */
    class CursorManager {
    public:
        virtual ~CursorManager() = default;

        /**
         * Initializes the object before the first cursors are actually opened. The resumeToken data
         * should be used to set the high-water mark in the 'AsyncResultsMerger' here in normal
         * mode, as well as the 'AsyncResultsMerger' can be told to recognize control events.
         */
        virtual void initialize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                ChangeStreamHandleTopologyChangeV2Stage* stage,
                                const ResumeTokenData& resumeTokenData) = 0;

        /**
         * Opens cursors on data-bearing shards using the given cluster time for the resume token.
         */
        virtual void openCursorsOnDataShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             OperationContext* opCtx,
                                             Timestamp atClusterTime,
                                             const stdx::unordered_set<ShardId>& shardIds) = 0;

        /**
         * Opens a cursor on the config server using the given cluster time for the resume token.
         */
        virtual void openCursorOnConfigServer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              OperationContext* opCtx,
                                              Timestamp atClusterTime) = 0;

        /**
         * Closes the cursors for the specified data shards.
         */
        virtual void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardIds) = 0;

        /**
         * Closes the cursor on the config server.
         */
        virtual void closeCursorOnConfigServer(OperationContext* opCtx) = 0;

        /**
         * Returns the shards ids of the currently targeted data shards.
         */
        virtual const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const = 0;

        /**
         * Returns the change stream object.
         */
        virtual const ChangeStream& getChangeStream() const = 0;

        /**
         * Returns the timestamp for the current high-water mark.
         */
        virtual Timestamp getTimestampFromCurrentHighWaterMark() const = 0;

        /**
         * Enables the undo-buffering in the underlying results merger of the mergeCursors stage.
         */
        virtual void enableUndoNextMode() = 0;

        /**
         * Disables the undo-buffering in the underlying results merger of the mergeCursors stage.
         */
        virtual void disableUndoNextMode() = 0;

        /**
         * Undoes the effects of the previous 'next()' call in the underlying results merger of the
         * mergeCursors stage and sets its high water mark to the value specified in
         * 'highWaterMark', using a high water mark token.
         */
        virtual void undoGetNextAndSetHighWaterMark(Timestamp highWaterMark) = 0;
    };

    /**
     * A struct containing the inputs for this stage as well as the concrete implementations of the
     * interfaces that are used by the stage.
     */
    struct Parameters {
        // The underlying change stream.
        ChangeStream changeStream;

        // The resumeToken when opening the change stream.
        ResumeTokenData resumeToken;

        // The minimum distance in time in seconds between two adjacent config server polls for the
        // historical shard placement information when the change stream is opened at a future time.
        int minAllocationToShardsPollPeriodSecs;

        // A concrete implementation for waiting until a deadline (used in state kWaiting).
        std::unique_ptr<DeadlineWaiter> deadlineWaiter;

        // A concrete implementation of the 'CursorManager'.
        std::shared_ptr<CursorManager> cursorManager;

        // A concrete 'ChangeStreamReaderBuilder' implementation. Not owned by the 'Parameters'
        // object! The pointed-to object must remain valid until the lifetime of the 'Parameters'
        // object ends.
        ChangeStreamReaderBuilder* changeStreamReaderBuilder;

        // A concrete 'DataToShardsAllocationQueryService' implementation. Not owned by the
        // 'Parameters' object! The pointed-to object must remain valid until the lifetime of the
        // 'Parameters' object ends.
        DataToShardsAllocationQueryService* dataToShardsAllocationQueryService;
    };

    ChangeStreamHandleTopologyChangeV2Stage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            std::shared_ptr<Parameters> params);

    /**
     * Returns a string representation of the state name.
     */
    static StringData stateToString(State state);

    /**
     * Returns the predecessor stage in the pipeline, which is always a 'MergeCursorsStage'.
     */
    exec::agg::MergeCursorsStage* getSourceStage() const;

    /**
     * Extracts the cluster timestamp from the input document, via the '_id' field.
     * Requires the document to contain an '_id' field with the proper contents.
     */
    static Timestamp extractTimestampFromDocument(const Document& input);

    // Test-only methods.
    // ------------------

    /**
     * Returns the '_lastAllocationToShardsRequestTime' value for testing.
     */
    Date_t getLastAllocationToShardsRequestTime_forTest() const {
        return _lastAllocationToShardsRequestTime;
    }

    /**
     * Sets the value of '_lastAllocationToShardsRequestTime' value for testing.
     */
    void setLastAllocationToShardsRequestTime_forTest(Date_t lastRequestTime) {
        _lastAllocationToShardsRequestTime = lastRequestTime;
    }

    /**
     * Returns the '_segmentStartTimestamp' value for testing.
     */
    const boost::optional<Timestamp>& getSegmentStartTimestamp_forTest() const {
        return _segmentStartTimestamp;
    }

    /**
     * Sets the value of '_segmentStartTimestamp' value for testing.
     */
    void setSegmentStartTimestamp_forTest(Timestamp ts) {
        _segmentStartTimestamp = ts;
    }

    /**
     * Returns the '_segmentEndTimestamp' value for testing.
     */
    const boost::optional<Timestamp>& getSegmentEndTimestamp_forTest() const {
        return _segmentEndTimestamp;
    }

    /**
     * Sets the value of '_segmentEndTimestamp' value for testing.
     */
    void setSegmentEndTimestamp_forTest(Timestamp ts) {
        _segmentEndTimestamp = ts;
    }

    /**
     * Returns the current state.
     */
    State getState_forTest() const {
        return _state;
    }

    /**
     * Injects the current start state for testing, and optionally validates the state transition.
     */
    void setState_forTest(State state, bool validateStateTransition);

    /**
     * Runs a single iteration of the internal state machine for testing.
     */
    boost::optional<DocumentSource::GetNextResult> runGetNextStateMachine_forTest();

    // Maximum number of 'ShardNotFound' errors in a row that the stage will accept in the
    // 'FetchingStartingChangeStreamSegment' state before running into a tassert. This is a
    // safeguard to prevent infinite fail loops.
    static constexpr int kMaxShardNotFoundFailuresInARow = 20;

private:
    /**
     * Produces the next document. Will dispatch to the internal state machine until there is
     * something to return or an error occurred.
     */
    GetNextResult doGetNext() final;

    /**
     * Runs a single iteration of the internal state machine.
     */
    boost::optional<DocumentSource::GetNextResult> _runGetNextStateMachine();

    /**
     * Sets the state to 'newState'.
     */
    void _setState(State newState);

    /**
     * Asserts that the current state is equal to 'expectedState' and the change stream's read mode
     * is equal to 'expectedMode' (if set). Will tassert otherwise.
     */
    void _assertState(State expectedState,
                      boost::optional<ChangeStreamReadMode> expectedMode,
                      StringData context) const;

    /**
     * Asserts that the change stream was opened in strict mode.
     */
    void _assertStrictMode(StringData context) const;

    /**
     * Ensures that the '_shardTargeter' instance variable is populated with a valid shard targeter
     * object.
     */
    void _ensureShardTargeter();

    /**
     * Logs the decision result of a ShardTargeter invocation for a specific context.
     * There are 3 variants of this function supporting slightly different additional parameters to
     * be logged.
     */
    void _logShardTargeterDecision(StringData context, ShardTargeterDecision targeterResult) const;
    void _logShardTargeterDecision(StringData context,
                                   ShardTargeterDecision targeterResult,
                                   const Document& event) const;
    void _logShardTargeterDecision(StringData context,
                                   ShardTargeterDecision targeterResult,
                                   Timestamp segmentBegin,
                                   Timestamp segmentEnd) const;

    /**
     * Queries the data-to-shards allocation status for the specified cluster time. Updates
     * '_lastAllocationToShardsRequestTime' with the current date/time as a side-effect.
     */
    AllocationToShardsStatus _getAllocationToShardsStatus(Timestamp clusterTime);

    boost::optional<DocumentSource::GetNextResult> _handleStateUninitialized();
    boost::optional<DocumentSource::GetNextResult> _handleStateWaiting();
    boost::optional<DocumentSource::GetNextResult> _handleStateFetchingInitialization();
    boost::optional<DocumentSource::GetNextResult> _handleStateFetchingGettingChangeEvent();
    boost::optional<DocumentSource::GetNextResult>
    _handleStateFetchingStartingChangeStreamSegment();
    boost::optional<DocumentSource::GetNextResult> _handleStateFetchingNormalGettingChangeEvent();
    boost::optional<DocumentSource::GetNextResult> _handleStateFetchingDegradedGettingChangeEvent();
    [[noreturn]] void _handleStateDowngrading();

    // Input parameters and dependencies for the stage.
    const std::shared_ptr<Parameters> _params;

    // The current state that the state machine for 'doGetNext()' and its callees is in.
    State _state = State::kUninitialized;

    // If an exception was caught during processing, the exception status will be recorded here, and
    // the same exception will be re-thrown for any further invocation of 'doGetNext()'.
    Status _lastError = Status::OK();

    // Contains the time of the last allocationToShards query that this stage has made.
    // This is used to throttle placement history requests on the config server.
    Date_t _lastAllocationToShardsRequestTime;

    // The number of "ShardNotFound' failures in a row that the stage has seen in the state
    // 'FetchingStartingChangeStreamSegment'. We are keeping track of this to be able to break out
    // of infinite fail loops.
    int _shardNotFoundFailuresInARow = 0;

    // Instance to determine the set of tracked shards and to manage (open/close) cursors to these
    // shards.
    std::unique_ptr<ChangeStreamShardTargeter> _shardTargeter;

    // Indicates the start time of the current change stream segment. Only used in
    // ignoreRemovedShards mode.
    boost::optional<Timestamp> _segmentStartTimestamp;

    // Indicates the end time of the current change stream segment. Only used in ignoreRemovedShards
    // mode.
    boost::optional<Timestamp> _segmentEndTimestamp;
};

}  // namespace mongo::exec::agg
