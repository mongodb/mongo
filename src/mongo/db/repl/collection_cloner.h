/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/callback_completion_guard.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/async_results_merger.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

class OldThreadPool;

namespace repl {

class StorageInterface;

class CollectionCloner : public BaseCloner {
    MONGO_DISALLOW_COPYING(CollectionCloner);

public:
    /**
     * Callback completion guard for CollectionCloner.
     */
    using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
    using OnCompletionGuard = CallbackCompletionGuard<Status>;

    struct Stats {
        static constexpr StringData kDocumentsToCopyFieldName = "documentsToCopy"_sd;
        static constexpr StringData kDocumentsCopiedFieldName = "documentsCopied"_sd;

        std::string ns;
        Date_t start;
        Date_t end;
        size_t documentToCopy{0};
        size_t documentsCopied{0};
        size_t indexes{0};
        size_t fetchBatches{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };
    /**
     * Type of function to schedule storage interface tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleDbWorkFn = stdx::function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        const executor::TaskExecutor::CallbackFn&)>;

    /**
     * Creates CollectionCloner task in inactive state. Use start() to activate cloner.
     *
     * The cloner calls 'onCompletion' when the collection cloning has completed or failed.
     *
     * 'onCompletion' will be called exactly once.
     *
     * Takes ownership of the passed StorageInterface object.
     */
    CollectionCloner(executor::TaskExecutor* executor,
                     OldThreadPool* dbWorkThreadPool,
                     const HostAndPort& source,
                     const NamespaceString& sourceNss,
                     const CollectionOptions& options,
                     const CallbackFn& onCompletion,
                     StorageInterface* storageInterface,
                     const int batchSize,
                     const int maxNumClonerCursors);

    virtual ~CollectionCloner();

    const NamespaceString& getSourceNamespace() const;

    bool isActive() const override;

    Status startup() noexcept override;

    void shutdown() override;

    void join() override;

    CollectionCloner::Stats getStats() const;

    //
    // Testing only functions below.
    //

    /**
     * Waits for database worker to complete.
     * Returns immediately if collection cloner is not active.
     *
     * For testing only.
     */
    void waitForDbWorker();

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn_forTest(const ScheduleDbWorkFn& scheduleDbWorkFn);

    /**
     * Returns the documents currently stored in the '_documents' buffer that is intended
     * to be inserted through the collection loader.
     *
     * For testing only.
     */
    std::vector<BSONObj> getDocumentsToInsert_forTest();

private:
    bool _isActive_inlock() const;

    /**
     * Returns whether the CollectionCloner is in shutdown.
     */
    bool _isShuttingDown() const;

    /**
     * Cancels all outstanding work.
     * Used by shutdown() and CompletionGuard::setResultAndCancelRemainingWork().
     */
    void _cancelRemainingWork_inlock();

    /**
     * Read number of documents in collection from count result.
     */
    void _countCallback(const executor::TaskExecutor::RemoteCommandCallbackArgs& args);

    /**
     * Read index specs from listIndexes result.
     */
    void _listIndexesCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                              Fetcher::NextAction* nextAction,
                              BSONObjBuilder* getMoreBob);

    /**
     * Request storage interface to create collection.
     *
     * Called multiple times if there are more than one batch of responses from listIndexes
     * cursor.
     *
     * 'nextAction' is an in/out arg indicating the next action planned and to be taken
     *  by the fetcher.
     */
    void _beginCollectionCallback(const executor::TaskExecutor::CallbackArgs& callbackData);

    /**
     * The possible command types that can be used to establish the initial cursors on the
     * remote collection.
     */
    enum EstablishCursorsCommand { Find, ParallelCollScan };

    /**
     * Parses the cursor responses from the 'find' or 'parallelCollectionScan' command
     * and passes them into the 'AsyncResultsMerger'.
     */
    void _establishCollectionCursorsCallback(const RemoteCommandCallbackArgs& rcbd,
                                             EstablishCursorsCommand cursorCommand);

    /**
     * Parses the response from a 'parallelCollectionScan' command into a vector of cursor
     * elements.
     */
    StatusWith<std::vector<BSONElement>> _parseParallelCollectionScanResponse(BSONObj resp);

    /**
     * Takes a cursors buffer and parses the 'parallelCollectionScan' response into cursor
     * responses that are pushed onto the buffer.
     */
    Status _parseCursorResponse(BSONObj response,
                                std::vector<CursorResponse>* cursors,
                                EstablishCursorsCommand cursorCommand);

    /**
     * Calls to get the next event from the 'AsyncResultsMerger'. This schedules
     * '_handleAsyncResultsCallback' to be run when the event is signaled successfully.
     */
    Status _scheduleNextARMResultsCallback(std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Runs for each time a new batch of documents can be retrieved from the 'AsyncResultsMerger'.
     * Buffers the documents retrieved for insertion and schedules a '_insertDocumentsCallback'
     * to insert the contents of the buffer.
     */
    void _handleARMResultsCallback(const executor::TaskExecutor::CallbackArgs& cbd,
                                   std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Pull all ready results from the ARM into a buffer to be inserted.
     */
    Status _bufferNextBatchFromArm(WithLock lock);

    /**
     * Called whenever there is a new batch of documents ready from the 'AsyncResultsMerger'.
     * On the last batch, 'lastBatch' will be true.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void _insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd,
                                  bool lastBatch,
                                  std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Verifies that an error from the ARM was the result of a collection drop.  If
     * so, cloning is stopped with no error.  Otherwise it is stopped with the given error.
     */
    void _verifyCollectionWasDropped(const stdx::unique_lock<stdx::mutex>& lk,
                                     Status batchStatus,
                                     std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                                     OperationContext* opCtx);

    /**
     * Reports completion status.
     * Commits/aborts collection building.
     * Sets cloner to inactive.
     */
    void _finishCallback(const Status& status);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (M)  Reads and writes guarded by _mutex
    // (S)  Self-synchronizing; access in any way from any context.
    // (RT)  Read-only in concurrent operation; synchronized externally by tests
    //
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condition;        // (M)
    executor::TaskExecutor* _executor;                  // (R) Not owned by us.
    OldThreadPool* _dbWorkThreadPool;                   // (R) Not owned by us.
    HostAndPort _source;                                // (R)
    NamespaceString _sourceNss;                         // (R)
    NamespaceString _destNss;                           // (R)
    CollectionOptions _options;                         // (R)
    std::unique_ptr<CollectionBulkLoader> _collLoader;  // (M)
    CallbackFn _onCompletion;             // (M) Invoked once when cloning completes or fails.
    StorageInterface* _storageInterface;  // (R) Not owned by us.
    RemoteCommandRetryScheduler _countScheduler;  // (S)
    Fetcher _listIndexesFetcher;                  // (S)
    std::vector<BSONObj> _indexSpecs;             // (M)
    BSONObj _idIndexSpec;                         // (M)
    std::vector<BSONObj>
        _documentsToInsert;        // (M) Documents read from 'AsyncResultsMerger' to insert.
    TaskRunner _dbWorkTaskRunner;  // (R)
    ScheduleDbWorkFn
        _scheduleDbWorkFn;         // (RT) Function for scheduling database work using the executor.
    Stats _stats;                  // (M) stats for this instance.
    ProgressMeter _progressMeter;  // (M) progress meter for this instance.
    const int _collectionCloningBatchSize;  // (R) The size of the batches of documents returned in
                                            // collection cloning.

    // (R) The maximum number of cursors to use in the collection cloning process.
    const int _maxNumClonerCursors;
    // (M) Component responsible for fetching the documents from the collection cloner cursor(s).
    std::unique_ptr<AsyncResultsMerger> _arm;
    // (R) The cursor parameters used by the 'AsyncResultsMerger'.
    std::unique_ptr<ClusterClientCursorParams> _clusterClientCursorParams;

    // (M) The event handle for the 'kill' event of the 'AsyncResultsMerger'.
    executor::TaskExecutor::EventHandle _killArmHandle;

    // (M) Scheduler used to establish the initial cursor or set of cursors.
    std::unique_ptr<RemoteCommandRetryScheduler> _establishCollectionCursorsScheduler;

    // (M) Scheduler used to determine if a cursor was closed because the collection was dropped.
    std::unique_ptr<RemoteCommandRetryScheduler> _verifyCollectionDroppedScheduler;

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the cloner has not started will transition from PreStart directly
    // to Complete.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };
    State _state = State::kPreStart;  // (M)
};

}  // namespace repl
}  // namespace mongo
