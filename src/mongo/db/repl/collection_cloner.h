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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
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
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/progress_meter.h"

namespace mongo {
namespace repl {

class StorageInterface;

class CollectionCloner : public BaseCloner {
    CollectionCloner(const CollectionCloner&) = delete;
    CollectionCloner& operator=(const CollectionCloner&) = delete;

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
        size_t fetchedBatches{0};  // This is actually inserted batches.
        size_t receivedBatches{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };
    /**
     * Type of function to schedule storage interface tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleDbWorkFn = unique_function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        executor::TaskExecutor::CallbackFn)>;

    /**
     * Type of function to create a database client
     *
     * Used for testing only.
     */
    using CreateClientFn = std::function<std::unique_ptr<DBClientConnection>()>;

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
                     ThreadPool* dbWorkThreadPool,
                     const HostAndPort& source,
                     const NamespaceString& sourceNss,
                     const CollectionOptions& options,
                     CallbackFn onCompletion,
                     StorageInterface* storageInterface,
                     const int batchSize);

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
    void setScheduleDbWorkFn_forTest(ScheduleDbWorkFn scheduleDbWorkFn);

    /**
     * Allows a different client class to be injected.
     *
     * For testing only.
     */
    void setCreateClientFn_forTest(const CreateClientFn& createClientFn);

    /**
     * Allows batch size to be changed after construction.
     *
     * For testing only.
     */
    void setBatchSize_forTest(int batchSize) {
        const_cast<int&>(_collectionClonerBatchSize) = batchSize;
    }

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
     */
    void _beginCollectionCallback(const executor::TaskExecutor::CallbackArgs& callbackData);

    /**
     * Using a DBClientConnection, executes a query to retrieve all documents in the collection.
     * For each batch returned by the upstream node, _handleNextBatch will be called with the data.
     * This method will return when the entire query is finished or failed.
     */
    void _runQuery(const executor::TaskExecutor::CallbackArgs& callbackData,
                   std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Put all results from a query batch into a buffer to be inserted, and schedule
     * it to be inserted.
     */
    void _handleNextBatch(std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                          DBClientCursorBatchIterator& iter);

    /**
     * Called whenever there is a new batch of documents ready from the DBClientConnection.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void _insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd,
                                  std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Verifies that an error from the query was the result of a collection drop.  If
     * so, cloning is stopped with no error.  Otherwise it is stopped with the given error.
     */
    void _verifyCollectionWasDropped(const stdx::unique_lock<stdx::mutex>& lk,
                                     Status batchStatus,
                                     std::shared_ptr<OnCompletionGuard> onCompletionGuard);

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
    ThreadPool* _dbWorkThreadPool;                      // (R) Not owned by us.
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
    std::vector<BSONObj> _documentsToInsert;      // (M) Documents read from source to insert.
    TaskRunner _dbWorkTaskRunner;                 // (R)
    ScheduleDbWorkFn
        _scheduleDbWorkFn;  // (RT) Function for scheduling database work using the executor.
    CreateClientFn _createClientFn;        // (RT) Function for creating a database client.
    Stats _stats;                          // (M) stats for this instance.
    ProgressMeter _progressMeter;          // (M) progress meter for this instance.
    const int _collectionClonerBatchSize;  // (R) The size of the batches of documents returned in
                                           // collection cloning.

    // (M) Scheduler used to determine if a cursor was closed because the collection was dropped.
    std::unique_ptr<RemoteCommandRetryScheduler> _verifyCollectionDroppedScheduler;

    // (M) State of query.  Set to kCanceling to cause query to stop. If the query is kRunning
    // or kCanceling, wait for query to reach kFinished using _condition.
    enum class QueryState {
        kNotStarted,
        kRunning,
        kCanceling,
        kFinished
    } _queryState = QueryState::kNotStarted;

    // (M) Client connection used for query. The '_clientConnection' is owned by the '_runQuery'
    // thread and may only be set by that thread, and only when holding '_mutex'. The '_runQuery'
    // thread may read this pointer without holding '_mutex'. It is exposed to other threads to
    // allow cancellation, and those other threads may access it only when holding '_mutex'.
    std::unique_ptr<DBClientConnection> _clientConnection;

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
