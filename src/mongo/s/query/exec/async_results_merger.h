/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/release_memory_gen.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/next_high_watermark_determining_strategy.h"
#include "mongo/s/query/exec/shard_tag.h"
#include "mongo/s/transaction_router.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/btree_set.h>
#include <absl/container/inlined_vector.h>
#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CursorResponse;

/**
 * Given a set of cursorIds across one or more shards, the AsyncResultsMerger calls getMore on the
 * cursors to present a single sorted or unsorted stream of documents.
 *
 * (A cursor-generating command (e.g. the find command) is one that establishes a ClientCursor and a
 * matching cursorId on the remote host. In order to retrieve all document results, getMores must be
 * issued against each of the remote cursors until they are exhausted).
 *
 * The ARM offers a non-blocking interface: if no results are immediately available on this host for
 * retrieval, calling nextEvent() schedules work on the remote hosts in order to generate further
 * results. The event is signaled when further results are available.
 *
 * Work on remote nodes is accomplished by scheduling remote work in TaskExecutor's event loop.
 *
 * Task-scheduling behavior differs depending on whether there is a sort. If the result documents
 * must be sorted, we pass the sort through to the remote nodes and then merge the sorted streams.
 * This requires waiting until we have a response from every remote before returning results.
 * Without a sort, we are ready to return results as soon as we have *any* response from a remote.
 *
 * On any error, the caller is responsible for shutting down the ARM using the kill() method.
 */
class AsyncResultsMerger : public std::enable_shared_from_this<AsyncResultsMerger> {
    AsyncResultsMerger(const AsyncResultsMerger&) = delete;
    AsyncResultsMerger& operator=(const AsyncResultsMerger&) = delete;

public:
    // When mongos has to do a merge in order to return results to the client in the correct sort
    // order, it requests a sortKey meta-projection using this field name.
    static constexpr StringData kSortKeyField = "$sortKey"_sd;

    // The expected sort key pattern when 'compareWholeSortKey' is true.
    static const BSONObj kWholeSortKeySortPattern;

    /**
     * Helper type for storing the additional transaction participants extracted from a shard
     * response. We need to first buffer the metadata about additional transaction participants when
     * we receive an async response in the network thread. This is because the async callback that
     * runs in the network thread cannot access the OperationContext nor the transaction router in a
     * safe way. The buffered metadata will be processed whenever the 'AsyncResultsMerger' is
     * detached from the OperationContext or when the next ready event is consumed.
     */
    struct RemoteResponse {
        ShardId shardId;
        TransactionRouter::ParsedParticipantResponseMetadata parsedMetadata;
    };

    /**
     * Factory function to create an 'AsyncResultsMerger' instance. Calling this method is the only
     * allowed way to create an 'AsyncResultsMerger'.
     *
     * Takes ownership of the cursors from ClusterClientCursorParams by storing their cursorIds and
     * the hosts on which they exist in '_remotes'.
     *
     * Additionally copies each remote's first batch of results, if one exists, into that remote's
     * docBuffer. If a sort is specified in the ClusterClientCursorParams, places the remotes with
     * buffered results onto _mergeQueue.
     *
     * The TaskExecutor* must remain valid for the lifetime of the ARM.
     *
     * If 'opCtx' may be deleted before this AsyncResultsMerger finishes, the caller must call
     * detachFromOperationContext() before deleting 'opCtx', and call reattachToOperationContext()
     * with a new, valid OperationContext before the next use.
     */
    static std::shared_ptr<AsyncResultsMerger> create(
        OperationContext* opCtx,
        std::shared_ptr<executor::TaskExecutor> executor,
        AsyncResultsMergerParams params);

    /**
     * In order to be destroyed, either the ARM must have been kill()'ed or all cursors must have
     * been exhausted. This is so that any unexhausted cursors are cleaned up by the ARM.
     */
    virtual ~AsyncResultsMerger();

    /**
     * Returns a const reference to the parameters.
     */
    const AsyncResultsMergerParams& params() const;

    /**
     * Returns true if all of the remote cursors are exhausted.
     */
    bool remotesExhausted() const;

    /**
     * Sets the maxTimeMS value that the ARM should forward with any internally issued getMore
     * requests.
     *
     * Returns a non-OK status if this cursor type does not support maxTimeMS on getMore (i.e. if
     * the cursor is not tailable + awaitData).
     */
    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout);

    /**
     * Signals to the AsyncResultsMerger that the caller is finished using it in the current
     * context.
     */
    void detachFromOperationContext();

    /**
     * Provides a new OperationContext to be used by the AsyncResultsMerger - the caller must call
     * detachFromOperationContext() before 'opCtx' is deleted.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Checks if the 'proposed' high water mark is greater or equal to 'current' high water mark,
     * w.r.t. to the 'sortKeyPattern'. Returns true if 'proposed' compares greater or equal to
     * 'current', false otherwise.
     */
    static bool checkHighWaterMarkIsMonotonicallyIncreasing(const BSONObj& current,
                                                            const BSONObj& proposed,
                                                            const BSONObj& sortKeyPattern);

    /**
     * Checks if the 'proposed' high water mark is less or equal to 'current' high water mark,
     * w.r.t. to the 'sortKeyPattern'. Returns true if 'proposed' compares less or equal to
     * 'current', false otherwise.
     */
    static bool checkHighWaterMarkIsMonotonicallyDecreasing(const BSONObj& current,
                                                            const BSONObj& proposed,
                                                            const BSONObj& sortKeyPattern);

    /**
     * Returns true if there is no need to schedule remote work in order to take the next action.
     * This means that either
     *   --there is a buffered result which we can return,
     *   --or all of the remote cursors have been closed and we are done,
     *   --or an error was received and the next call to nextReady() will return an error status,
     *   --or the ARM has been killed and is in the process of shutting down. In this case,
     *   nextReady() will report an error when called.
     *
     * A return value of true indicates that it is safe to call nextReady().
     */
    bool ready();

    /**
     * If there is a result available that has already been retrieved from a remote node and
     * buffered, then return it along with an ok status.
     *
     * If we have reached the end of the stream of results, returns boost::none along with an ok
     * status.
     *
     * If this AsyncResultsMerger is fetching results from a remote cursor tailing a capped
     * collection, may return an empty ClusterQueryResult before end-of-stream. (Tailable cursors
     * remain open even when there are no further results, and may subsequently return more results
     * when they become available.) The calling code is responsible for handling multiple empty,
     * ClusterQueryResult return values, keeping the cursor open in the tailable case.
     *
     * If there has been an error received from one of the shards, or there is an error in
     * processing results from a shard, then a non-ok status is returned.
     *
     * Invalid to call unless ready() has returned true (i.e., invalid to call if getting the next
     * result requires scheduling remote work).
     */
    StatusWith<ClusterQueryResult> nextReady();

    /**
     * Schedules remote work as required in order to make further results available. If there is an
     * error in scheduling this work, returns a non-ok status. On success, returns an event handle.
     * The caller can pass this event handle to 'executor' in order to be blocked until further
     * results are available.
     *
     * Invalid to call unless ready() has returned false (i.e. invalid to call if the next result is
     * available without scheduling remote work).
     *
     * Also invalid to call if there is an outstanding event, created by a previous call to this
     * function, that has not yet been signaled. If there is an outstanding unsignaled event,
     * returns an error.
     *
     * If there is a sort, the event is signaled when there are buffered results for all
     * non-exhausted remotes.
     * If there is no sort, the event is signaled when some remote has a buffered result.
     */
    StatusWith<executor::TaskExecutor::EventHandle> nextEvent();

    /**
     * Schedules a getMore on any remote hosts which:
     *  - Do not have an error status set already.
     *  - Don't already have a request outstanding.
     *  - We don't currently have any results buffered.
     *  - Are not exhausted (have a non-zero cursor id).
     * Returns an error if any of the remotes responded with an error, or if we encounter an error
     * while scheduling the getMore requests..
     *
     * In most cases users should call nextEvent() instead of this method, but this can be necessary
     * if the caller of nextEvent() calls detachFromOperationContext() before the event is signaled.
     * In such cases, the ARM cannot schedule getMores itself, and will need to be manually prompted
     * after calling reattachToOperationContext().
     *
     * It is illegal to call this method if the ARM is not attached to an OperationContext.
     */
    Status scheduleGetMores();

    Status releaseMemory();

    /**
     * Adds the specified shard cursors to the set of cursors to be merged.  The results from the
     * new cursors will be returned as normal through nextReady().
     */
    void addNewShardCursors(std::vector<RemoteCursor>&& newCursors,
                            const ShardTag& tag = ShardTag::kDefault);

    /**
     * Closes and removes all cursors belonging to any of the specified shardIds. All in-flight
     * requests to any of these remote cursors will be canceled and discarded.
     * All results from the to-be closed remotes that have already been received by this
     * 'AsyncResultsMerger' but have not been consumed will be kept. They can be consumed normally
     * via the 'nextReady()' method.
     * Closing remote cursors is only supported for tailable, awaitData cursors.
     */
    void closeShardCursors(const stdx::unordered_set<ShardId>& shardIds, const ShardTag& tag);

    /**
     * Returns true if the cursor was opened with 'allowPartialResults:true' and results are not
     * available from one or more shards.
     */
    bool partialResultsReturned() const;

    /**
     * Returns the number of remotes involved in this operation.
     */
    std::size_t getNumRemotes() const;

    /**
     * Returns true if we have a cursor established for the specified shard and shard tag, false
     * otherwise.
     */
    bool hasCursorForShard_forTest(const ShardId& shardId,
                                   const ShardTag& tag = ShardTag::kDefault) const;

    /**
     * Returns the number of buffered remote responses.
     */
    std::size_t numberOfBufferedRemoteResponses_forTest() const;

    /**
     * For sorted tailable cursors, returns the most recent available sort key. This guarantees that
     * we will never return any future results which precede this key. If no results are ready to be
     * returned, this method may cause the high water mark to advance to the lowest promised sortkey
     * received from the shards. Returns an empty BSONObj if no such sort key is available.
     */
    BSONObj getHighWaterMark();

    /**
     * Sets the initial high watermark to return when no cursors are tracked. It is not allowed to
     * call this method with a high water mark with a timestamp earlier than the current high water
     * mark.
     */
    void setInitialHighWaterMark(const BSONObj& highWaterMark);

    /**
     * Sets the current high water mark of the 'AsyncResultsMerger'. Notably this allows to set the
     * high water mark to a timestamp earlier than the current high water mark.
     */
    void setHighWaterMark(const BSONObj& highWaterMark);

    /**
     * Sets the strategy to determine the next high water mark.
     * Assumes that the 'AsyncResultsMerger' is in tailable, awaitData mode.
     */
    void setNextHighWaterMarkDeterminingStrategy(
        NextHighWaterMarkDeterminingStrategyPtr nextHighWaterMarkDeterminingStrategy);

    /**
     * Enables an undo mode in which the effects of the last 'nextReady()' call can be partially
     * reverted. In this mode, the 'AsyncResultsMerger' keeps track of the last result that was
     * returned by a call to 'nextReady()'. The effects of fetching the last result via
     * 'nextReady()' can then be "undone" by calling 'undoNextReady()'.
     */
    void enableUndoNextReadyMode();

    /**
     * Disables the undo mode. Also clears a buffered undo result.
     */
    void disableUndoNextReadyMode();

    /**
     * Partially "undoes" the effects of the last 'nextReady()' call, by putting the last returned
     * result back into the 'AsyncResultMerger's document buffer for the shard it was originally
     * pulled from. For sorted mergers, it will also put back the result back into the merge queue
     * and restore the high water mark.
     *
     * This method can only be called if a result was previously returned via 'nextReady()' while
     * the undo mode was enabled. Calling it otherwise will tassert. As the 'AsyncResultsMerger'
     * only buffers the single last returned result in 'nextReady()', it is forbidden to call
     * 'undoNextReady()' repeatedly unless another call to 'nextReady()' has been made. If this
     * assumption is violated, this method will tassert.
     *
     * Calling this method will not restore the following properties of the 'AsyncResultsMerger' to
     * their state during the last call to 'nextReady()':
     * - the set of open remote cursors
     * - the list of transaction participants
     *
     * It is forbidden to call this method if the undo is performed for a result that was produced
     * by a remote cursor that has been closed since the last call to 'nextReady()'. If this
     * assumption is violated, this method will tassert.
     */
    void undoNextReady();

    /**
     * Returns if this 'AsyncResultsMerger' compares the whole sort key, based on the initial setup
     * parameters.
     */
    bool getCompareWholeSortKey() const;

    /**
     * Starts shutting down this ARM by canceling all pending requests and scheduling killCursors
     * on all of the unexhausted remotes. Returns a 'future' that is signaled when this ARM is safe
     * to destroy.
     *
     * If there are no pending requests, schedules killCursors and signals the future immediately.
     * Otherwise, the last callback that runs after kill() is called signals the event.
     *
     * May be called multiple times (idempotent).
     *
     * Note that 'opCtx' may or may not be the same as the operation context to which this cursor is
     * currently attached. This is so that a killing thread may call this method with its own
     * operation context.
     */
    stdx::shared_future<void> kill(OperationContext* opCtx);

    /**
     * Returns remote metrics aggregated in this ARM without reseting the local counts.
     */
    const query_stats::DataBearingNodeMetrics& peekMetrics_forTest() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _metrics;
    }

    /**
     * Returns remotes metrics aggregated in this ARM and resets the local counts so as to avoid
     * double counting.
     */
    query_stats::DataBearingNodeMetrics takeMetrics();

    /**
     * Returns the sort key out of the $sortKey metadata field in 'obj'. The sort key should be
     * formatted as an array with one value per field of the sort pattern:
     *  {..., $sortKey: [<firstSortKeyComponent>, <secondSortKeyComponent>, ...], ...}
     *
     * This function returns the sort key not as an array, but as the equivalent BSONObj:
     *   {"0": <firstSortKeyComponent>, "1": <secondSortKeyComponent>}
     *
     * The return value is allowed to omit the key names, so the caller should not rely on the key
     * names being present. That is, the return value could consist of an object such as
     *   {"": <firstSortKeyComponent>, "": <secondSortKeyComponent>}
     *
     * If 'compareWholeSortKey' is true, then the value inside the $sortKey is directly interpreted
     * as a single-element sort key. For example, given the document
     *   {..., $sortKey: <value>, ...}
     * and 'compareWholeSortKey'=true, this function will return
     *   {"": <value>}
     */
    static BSONObj extractSortKey(const BSONObj& obj, bool compareWholeSortKey);

private:
    /**
     * Constructor is private. All 'AsyncResultsMerger' objects are supposed to be created via the
     * static 'create()' factory function.
     */
    AsyncResultsMerger(OperationContext* opCtx,
                       std::shared_ptr<executor::TaskExecutor> executor,
                       AsyncResultsMergerParams params);

    /**
     * One 'RemoteCursorData' instance is instantiated per remote host. A 'RemoteCursorData'
     * contains the buffer of results that have been retrieved from the host but not yet returned,
     * as well as the cursor id, and any error reported by the remote.
     */
    struct RemoteCursorData : public RefCountable {
        /**
         * Type used for the internal ids of RemoteCursorData objects.
         */
        using IdType = long long;

        RemoteCursorData(IdType id,
                         HostAndPort hostAndPort,
                         NamespaceString cursorNss,
                         CursorId establishedCursorId,
                         std::string shardId,
                         const ShardTag& tag,
                         bool partialResultsReturned);

        /**
         * Returns the resolved host and port on which the remote cursor resides.
         */
        const HostAndPort& getTargetHost() const;

        /**
         * Returns whether there is another buffered result available for this remote node.
         */
        bool hasNext() const;

        /**
         * Returns whether the remote has given us all of its results (i.e. whether it has closed
         * its cursor).
         */
        bool exhausted() const;

        /**
         * Cleans up the RemoteCursor internals after receiving more data failed.
         */
        void cleanUpFailedBatch(Status status, bool allowPartialResults);

        /**
         * Internal id of the RemoteCursorData object. The id is guaranteed to be unique for
         * different 'RemoteCursorData' objects managed by the same 'AsyncResultsMerger', and can be
         * used for comparison purposes. The id is not necessarily unique across different
         * 'AsyncResultsMerger' instances.
         */
        const IdType id;

        // Used when merging tailable awaitData cursors in sorted order. In order to return any
        // result to the client we have to know that no shard will ever return anything that sorts
        // before it. This object represents a promise from the remote that it will never return a
        // result with a sort key lower than this.
        boost::optional<BSONObj> promisedMinSortKey;

        // The cursor id for the remote cursor. If a remote cursor is not yet exhausted, this member
        // will be set to a valid non-zero cursor id. If a remote cursor is now exhausted, this
        // member will be set to zero.
        CursorId cursorId;

        // The namespace this cursor belongs to - note this may be different than the namespace of
        // the operation if there is a view.
        const NamespaceString cursorNss;

        // The exact host in the shard on which the cursor resides. Can be empty if this merger has
        // 'allowPartialResults' set to true and initial cursor establishment failed on this shard.
        const HostAndPort shardHostAndPort;

        // The identity of the shard which the cursor belongs to.
        const ShardId shardId;

        // The shard tag associated with this cursor.
        const ShardTag tag;

        // True if this remote is eligible to provide a high water mark sort key; false otherwise.
        bool eligibleForHighWaterMark = false;

        // This flag is set if the connection to the remote shard was lost or never established in
        // the first place or the connection is interrupted due to MaxTimeMSExpired.
        // Only applicable if the 'allowPartialResults' option is enabled.
        bool partialResultsReturned = false;

        // If set to 'true', the cursor on this shard has been invalidated.
        bool invalidated = false;

        // Set to 'true' once the remote cursor was closed via a call to 'closeShardCursors()'.
        // Cannot flip back from 'true' to 'false'.
        bool closed = false;

        // The buffer of results that have been retrieved but not yet returned to the caller.
        std::deque<BSONObj> docBuffer;

        // Is valid if there is currently a pending request to this remote.
        executor::TaskExecutor::CallbackHandle cbHandle;

        // Set to an error status if there is an error retrieving a response from this remote or if
        // the command result contained an error.
        Status status = Status::OK();
    };

    using RemoteCursorPtr = boost::intrusive_ptr<RemoteCursorData>;

    /**
     * The type that is returned by the internal helper functions for 'nextReady()'.
     * In case the undo mode is disabled, only the 'ClusterQueryResult' part will be populated. In
     * case undo mode is enabled, the 'RemoteCursorPtr' part contains the remote from which the
     * result was fetched, and the BSONObj contains the high water mark.
     */
    using NextReadyResult = std::tuple<ClusterQueryResult, RemoteCursorPtr, BSONObj>;

    class MergingComparator {
    public:
        MergingComparator(const BSONObj& sort, bool compareWholeSortKey)
            : _sort(sort), _compareWholeSortKey(compareWholeSortKey) {}

        bool operator()(const RemoteCursorPtr& lhs, const RemoteCursorPtr& rhs) const;

    private:
        BSONObj _sort;

        // When '_compareWholeSortKey' is true, $sortKey is a scalar value, rather than an object.
        // We extract the sort key {$sortKey: <value>}. The sort key pattern '_sort' is verified to
        // be {$sortKey: 1}.
        bool _compareWholeSortKey;
    };

    using MinSortKeyRemotePair = std::pair<BSONObj, RemoteCursorPtr>;

    /**
     * Custom comparator type used by 'AsyncResultsMerger::_promisedMinSortKeys'.
     */
    class PromisedMinSortKeyComparator {
    public:
        PromisedMinSortKeyComparator(BSONObj sort) : _sort(std::move(sort)) {}

        // The copy constructor needs to be declared noexcept because 'absl::btree_set' requires the
        // set's comparator type to be nothrow-copy-constructible.
        PromisedMinSortKeyComparator(const PromisedMinSortKeyComparator&) noexcept = default;

        bool operator()(const MinSortKeyRemotePair& lhs, const MinSortKeyRemotePair& rhs) const;

    private:
        BSONObj _sort;
    };

    enum LifecycleState { kAlive, kKillStarted, kKillComplete };

    /**
     * Ensures that the high watermark token in 'proposed' compares equal or higher to the high
     * watermark token in 'proposed', compared to the internal sort key pattern. Tasserts if this
     * assumption is violated.
     */
    void _ensureHighWaterMarkIsMonotonicallyIncreasing(const BSONObj& current,
                                                       const BSONObj& proposed,
                                                       StringData context) const;

    /**
     * Ensures that the high watermark token in 'proposed' compares equal or less to the high
     * watermark token in 'proposed', compared to the internal sort key pattern. Tasserts if this
     * assumption is violated.
     */
    void _ensureHighWaterMarkIsMonotonicallyDecreasing(const BSONObj& current,
                                                       const BSONObj& proposed,
                                                       StringData context) const;

    /**
     * Internal helper function to set the high water mark. If 'mustBeMonotonicallyIncreasing' is
     * set to 'true', will validate that the timestamp contained in the high water mark is not be
     * less than that of the current high water mark contained in '_highWaterMark'. If
     * 'mustBeMonotonicallyIncreasing' is set to 'false', will validate that the timestamp contained
     * in the high water mark will not be greater than that of the current high water mark.
     */
    void _setHighWaterMark(const BSONObj& highWaterMark,
                           StringData context,
                           bool mustBeMonotonicallyIncreasing);

    /**
     * Parses the find or getMore command response object to a CursorResponse.
     *
     * Returns a non-OK response if the response fails to parse or if there is a cursor id mismatch.
     */
    static StatusWith<CursorResponse> _parseCursorResponse(const BSONObj& responseObj,
                                                           CursorId expectedCursorId);

    /**
     * Helper to create the getMore command asking the remote node for another batch of results.
     */
    BSONObj _makeRequest(WithLock,
                         const RemoteCursorData& remote,
                         const ServerGlobalParams::FCVSnapshot& fcvSnapshot) const;

    /**
     * Updates the internal high water mark with the passed value, using the configured next high
     * watermark determining strategy.
     */
    void _updateHighWaterMark(const BSONObj& value);

    /**
     * Checks whether or not the remote cursors are all exhausted.
     */
    bool _remotesExhausted(WithLock) const;

    //
    // Helpers for ready().
    //

    bool _ready(WithLock) const;
    bool _readySorted(WithLock) const;
    bool _readySortedTailable(WithLock) const;
    bool _readyUnsorted(WithLock) const;

    //
    // Helpers for nextReady().
    //

    NextReadyResult _nextReadySorted(WithLock);
    NextReadyResult _nextReadyUnsorted(WithLock);

    using CbData = executor::TaskExecutor::RemoteCommandCallbackArgs;
    using CbResponse = executor::TaskExecutor::ResponseStatus;

    /**
     * Builds a remote cursor object from the 'RemoteCursor' and 'ShardTag' objects.
     */
    RemoteCursorPtr _buildRemote(WithLock lk, const RemoteCursor& rc, const ShardTag& tag);

    /**
     * When nextEvent() schedules remote work, the callback uses this function to process
     * results.
     */
    void _handleBatchResponse(WithLock lk,
                              CbData const&,
                              StatusWith<CursorResponse>& response,
                              const RemoteCursorPtr& remote);

    /**
     * Schedules a killCursors request for the remote if the remote still has a cursor open.
     * This is a fire-and-forget attempt to close the remote cursor. We are not blocking until
     * the remote cursor is actually closed.
     */
    void _scheduleKillCursorForRemote(WithLock lk,
                                      OperationContext* opCtx,
                                      const RemoteCursorPtr& remote);

    /**
     * Cleans up if the remote cursor was killed while waiting for a response.
     */
    void _cleanUpKilledBatch(WithLock);

    /**
     * Cleans up after remote query failure.
     */
    void _cleanUpFailedBatch(WithLock, Status status, RemoteCursorData& remote);

    /**
     * Processes results from a remote query.
     */
    void _processBatchResults(WithLock lk,
                              const CursorResponse& cursorResponse,
                              const RemoteCursorPtr& remote);

    /**
     * Adds the batch of results to the RemoteCursorData. Returns false if there was an error
     * parsing the batch.
     */
    bool _addBatchToBuffer(WithLock lk,
                           const RemoteCursorPtr& remote,
                           const CursorResponse& response);

    /**
     * If there is a valid unsignaled event that has been requested via nextEvent() and there
     * are buffered results that are ready to return, signals that event.
     *
     * Invalidates the current event, as we must signal the event exactly once and we only keep
     * a handle to a valid event if it is unsignaled.
     */
    void _signalCurrentEventIfReady(WithLock);

    /**
     * Returns true if this async cursor is waiting to receive another batch from a remote.
     */
    bool _haveOutstandingBatchRequests(WithLock);

    /**
     * Called internally when attempting to get a new event for the caller to wait on. Throws if
     * the shard cursor from which the next result is due has already been invalidated.
     */
    void _assertNotInvalidated(WithLock);

    /**
     * If a promisedMinSortKey has been received from all remotes, returns the lowest such key.
     * Otherwise, returns boost::none.
     */
    boost::optional<MinSortKeyRemotePair> _getMinPromisedSortKey(WithLock) const;

    /**
     * Schedules a getMore on any remote hosts which we need another batch from.
     */
    Status _scheduleGetMores(WithLock);

    /**
     * Schedules a getMore on all remote hosts passed in the 'remotes' vector.
     */
    Status _scheduleGetMoresForRemotes(WithLock lk,
                                       OperationContext* opCtx,
                                       const std::vector<RemoteCursorPtr>& remotes);

    /**
     * Schedules a killCursors command to be run on all remote hosts that have open cursors.
     */
    void _scheduleKillCursors(WithLock, OperationContext* opCtx);

    /**
     * Checks if we need to schedule a killCursor command for this remote
     */
    bool _shouldKillRemote(WithLock, const RemoteCursorData& remote);

    /**
     * Updates the given remote's metadata (e.g. the cursor id) based on information in
     * 'response'.
     */
    void _updateRemoteMetadata(WithLock,
                               const RemoteCursorPtr& remote,
                               const CursorResponse& response);

    /**
     * Returns true if the given batch is eligible to provide a high water mark resume token for the
     * stream, false otherwise.
     */
    bool _checkHighWaterMarkEligibility(WithLock,
                                        BSONObj newMinSortKey,
                                        const RemoteCursorData& remote,
                                        const CursorResponse& response) const;

    /**
     * Sets the initial value of the high water mark sort key, if applicable.
     */
    void _determineInitialHighWaterMark();

    /**
     * Processes additional participants received in the responses if necessary.
     */
    void _processAdditionalTransactionParticipants(OperationContext* opCtx);

    /**
     * Removes a remote from the _promisedMinSortKeys set, if already present in there.
     */
    void _removeRemoteFromPromisedMinSortKeys(WithLock lk, const RemoteCursorPtr& remote);

    /**
     * Rebuilds the merge queue for the remaining remotes. This is supposed to be called internally
     * after closing cursors.
     */
    void _rebuildMergeQueueFromRemainingRemotes(WithLock lk);

    /**
     * Cancels any potential in-flight callback for the remote.
     */
    void _cancelCallbackForRemote(WithLock lk, const RemoteCursorPtr& remote);

    OperationContext* _opCtx;
    std::shared_ptr<executor::TaskExecutor> _executor;

    const AsyncResultsMergerParams _params;
    const TailableModeEnum _tailableMode;

    // Must be acquired before accessing any non-const data members.
    mutable stdx::mutex _mutex;

    // Metrics aggregated from remote cursors.
    query_stats::DataBearingNodeMetrics _metrics;

    /**
     * Id for the next created 'RemoteCursorData' object to be managed by this 'AsyncResultsMerger'
     * instance. Id values are guaranteed to be unique for all 'RemoteCursorData' instances managed
     * by the same 'AsyncResultsMerger'.
     *
     * Currently the generated ids are only used when there are multiple remote cursors which have a
     * next buffered document that compares equal. In this case we need another attribute to break
     * ties for a deterministic sort order, and we use the internal id for this. This is better
     * than comparing the pointers of the objects, as the pointer values are less predictable
     * for example in unit tests.
     */
    RemoteCursorData::IdType _nextId = 0;

    /**
     * Default inline allocation size for the '_remotes' vector. Should be enough for small-size
     * deployments to never allocate memory on the heap for storing the pointers of remotes.
     */
    static constexpr auto kDefaultRemotesContainerSize = 8;

    /**
     * The container tracking all currently active remotes. Additional remotes can be added to the
     * container via 'addNewShardCursors()', and remotes can be removed via 'closeShardCursors()'.
     */
    absl::InlinedVector<RemoteCursorPtr, kDefaultRemotesContainerSize> _remotes;

    /**
     * List of pending responses to be processed for additional participants. Remote responses are
     * buffered here until they are processed in 'nextReady()' or 'detachedFromOperationContext()'.
     */
    std::queue<RemoteResponse> _remoteResponses;

    /**
     * The top of this priority queue is the remote host that has the next document to return,
     * according to the sort order. Used only if there is a sort.
     *
     * Note that the priority queue can contain remotes which have already been removed via a call
     * to 'closeShardCursors()', if there are still buffered and unprocessed responses for that
     * remote.
     */
    std::priority_queue<RemoteCursorPtr,
                        absl::InlinedVector<RemoteCursorPtr, kDefaultRemotesContainerSize>,
                        MergingComparator>
        _mergeQueue;

    /**
     * The index into the '_remotes' vector for the remote from which we are currently retrieving
     * results. Used only if there is *not* a sort. This is used to implement a sort of round-robin
     * pulling from multiple remotes. The algorithm does not guarantee any strict order, and when
     * removing a remote, it may start from node 0 again. This is fine however if no particular sort
     * order was requested.
     */
    size_t _gettingFromRemote = 0;

    /**
     * Overall status of this 'AsyncResultsMerger'. Will be updated to a non-OK status if any of the
     * remotes returns a non-OK status, and will never transition back from non-OK to OK.
     */
    Status _status = Status::OK();

    executor::TaskExecutor::EventHandle _currentEvent;

    boost::optional<Milliseconds> _awaitDataTimeout;

    /**
     * An ordered set of (promisedMinSortKey, remote) pairs received from the shards. The first
     * element in the set will be the lowest sort key across all shards.
     * We use the lowest of these keys as a high watermark.
     */
    absl::btree_set<MinSortKeyRemotePair, PromisedMinSortKeyComparator> _promisedMinSortKeys;

    // For sorted tailable cursors, records the current high-water-mark sort key. Empty
    // otherwise.
    BSONObj _highWaterMark;

    // Strategy for determining the next high watermark in tailable, awaitData mode. Not used in
    // other modes.
    NextHighWaterMarkDeterminingStrategyPtr _nextHighWaterMarkDeterminingStrategy;

    // For tailable cursors, set to true if the next result returned from nextReady() should be
    // boost::none. Can only ever be true for 'TailableModeEnum::kTailable' cursors, but not for
    // other cursor types.
    bool _eofNext = false;

    /**
     * True if the undo mode is enabled and the results merger stores the last returned result of
     * 'nextReady()' so it can be undone later.
     */
    bool _undoModeEnabled = false;

    /**
     * State required to undo a previous invocation of the function 'nextReady()'. Will only be
     * populated if '_undoModeEnabled' is true and a call to 'nextReady()' has been performed.
     */
    boost::optional<NextReadyResult> _stateForNextReadyCallUndo;

    //
    // Killing
    //

    LifecycleState _lifecycleState = kAlive;

    // Handles the promise/future mechanism used to cleanly shut down the ARM. This avoids race
    // conditions in cases where the underlying TaskExecutor is simultaneously being torn down.
    struct CompletePromiseFuture {
        CompletePromiseFuture() : _future(_promise.get_future()) {}

        // Multiple calls to the method that creates the promise (i.e., kill()) can
        // be made and each must return a future that will be notified when the ARM has been cleaned
        // up.
        stdx::shared_future<void> getFuture() {
            return _future;
        }

        // Called by the ARM when all outstanding requests have run. Notifies all threads
        // waiting on shared futures that the ARM has been cleaned up.
        void signalFutures() {
            _promise.set_value();
        }

    private:
        std::promise<void> _promise;  // NOLINT needs audit
        std::shared_future<void> _future;
    };

    boost::optional<CompletePromiseFuture> _killCompleteInfo;
};

}  // namespace mongo
