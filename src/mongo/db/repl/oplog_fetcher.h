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

#include <cstddef>
#include <functional>

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/abstract_async_component.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace repl {

extern FailPoint stopReplProducer;

/**
 * The oplog fetcher, once started, reads operations from a remote oplog using a tailable,
 * awaitData, exhaust cursor.
 *
 * The initial `find` command is generated from the last fetched optime.
 *
 * Using RequestMetadataWriter and ReplyMetadataReader, the sync source will forward metadata in
 * each response that will be sent to the data replicator external state.
 *
 * Performs additional validation on first batch of operations returned from the query to ensure we
 * are able to continue from our last known fetched operation.
 *
 * Validates each batch of operations to make sure that none of the oplog entries are out of order.
 *
 * Collect stats about all the batches received to be able to report in serverStatus metrics.
 *
 * Pushes operations from each batch of operations onto a buffer using the "enqueueDocumentsFn"
 * function.
 *
 * When there is an error, it will create a new cursor by issuing a new `find` command to the sync
 * source. If the sync source is no longer eligible or the OplogFetcher was shutdown, calls
 * "onShutdownCallbackFn" to signal the end of processing.
 *
 * An oplog fetcher is an abstract async component, which takes care of startup and shutdown logic.
 */
class OplogFetcher : public AbstractAsyncComponent {
    OplogFetcher(const OplogFetcher&) = delete;
    OplogFetcher& operator=(const OplogFetcher&) = delete;

public:
    /**
     * Type of function called by the oplog fetcher on shutdown with the final oplog fetcher status.
     *
     * The status will be Status::OK() if we have processed the last batch of operations from the
     * cursor.
     *
     * rbid will be set to the rollback id of the oplog query metadata for the first batch fetched
     * from the sync source.
     *
     * This function will be called 0 times if startup() fails and at most once after startup()
     * returns success.
     */
    using OnShutdownCallbackFn = std::function<void(const Status& shutdownStatus, int rbid)>;

    /**
     * Container for BSON documents extracted from cursor results.
     */
    using Documents = std::vector<BSONObj>;

    /**
     * An enum that indicates if we want to skip the first document during oplog fetching or not.
     * Currently, the only time we don't want to skip the first document is during initial sync
     * if the sync source has a valid oldest active transaction optime, as we need to include
     * the corresponding oplog entry when applying.
     */
    enum class StartingPoint { kSkipFirstDoc, kEnqueueFirstDoc };

    /**
     * Statistics on current batch of operations returned by the sync source.
     */
    struct DocumentsInfo {
        size_t networkDocumentCount = 0;
        size_t networkDocumentBytes = 0;
        size_t toApplyDocumentCount = 0;
        size_t toApplyDocumentBytes = 0;
        OpTime lastDocument = OpTime();
        Timestamp resumeToken = Timestamp();
    };

    /**
     * Type of function that accepts a pair of iterators into a range of operations
     * within the current batch of results and copies the operations into
     * a buffer to be consumed by the next stage of the replication process.
     *
     * Additional information on the operations is provided in a DocumentsInfo
     * struct.
     */
    using EnqueueDocumentsFn = std::function<Status(
        Documents::const_iterator begin, Documents::const_iterator end, const DocumentsInfo& info)>;

    class OplogFetcherRestartDecision {
    public:
        OplogFetcherRestartDecision(){};

        virtual ~OplogFetcherRestartDecision() = 0;

        /**
         * Defines which situations the oplog fetcher will restart after encountering an error.
         * Called when getting the next batch failed for some reason.
         */
        virtual bool shouldContinue(OplogFetcher* fetcher, Status status) = 0;

        /**
         * Called when a batch was successfully fetched to reset any state needed to track restarts.
         */
        virtual void fetchSuccessful(OplogFetcher* fetcher) = 0;
    };

    class OplogFetcherRestartDecisionDefault : public OplogFetcherRestartDecision {
    public:
        OplogFetcherRestartDecisionDefault(std::size_t maxRestarts) : _maxRestarts(maxRestarts){};

        bool shouldContinue(OplogFetcher* fetcher, Status status) final;

        void fetchSuccessful(OplogFetcher* fetcher) final;

        ~OplogFetcherRestartDecisionDefault(){};

    private:
        // Restarts since the last successful oplog query response.
        std::size_t _numRestarts = 0;

        const std::size_t _maxRestarts;
    };

    enum class RequireFresherSyncSource {
        kDontRequireFresherSyncSource,
        kRequireFresherSyncSource
    };

    struct Config {
        Config(OpTime initialLastFetchedIn,
               HostAndPort sourceIn,
               ReplSetConfig replSetConfigIn,
               int requiredRBIDIn,
               int batchSizeIn,
               RequireFresherSyncSource requireFresherSyncSourceIn =
                   RequireFresherSyncSource::kRequireFresherSyncSource,
               bool forTenantMigrationIn = false)
            : initialLastFetched(initialLastFetchedIn),
              source(sourceIn),
              replSetConfig(replSetConfigIn),
              requiredRBID(requiredRBIDIn),
              batchSize(batchSizeIn),
              requireFresherSyncSource(requireFresherSyncSourceIn),
              forTenantMigration(forTenantMigrationIn) {}
        // The OpTime, last oplog entry fetched in a previous run, or the optime to start fetching
        // from, depending on the startingPoint (below.).  If the startingPoint is kSkipFirstDoc,
        // this entry will be verified to exist, then discarded. If it is kEnqueueFirstDoc, it will
        // be sent to the enqueue function with the first batch.
        OpTime initialLastFetched;

        // Sync source to read from.
        HostAndPort source;

        ReplSetConfig replSetConfig;

        // Rollback ID that the sync source is required to have after the first batch. If the value
        // is uninitialized, the oplog fetcher has not contacted the sync source yet.
        int requiredRBID;

        int batchSize;

        // A flag indicating whether we should error if the sync source is not ahead of our initial
        // last fetched OpTime on the first batch. Most of the time this should be set to
        // kRequireFresherSyncSource, but there are certain special cases where it's acceptable for
        // our sync source to have no ops newer than _lastFetched.
        RequireFresherSyncSource requireFresherSyncSource;

        // Predicate with additional filtering to be done on oplog entries.
        BSONObj queryFilter = BSONObj();

        // Read concern to use for reading the oplog.  Empty read concern means we use a default
        // of "afterClusterTime: Timestamp(0,1)".
        ReadConcernArgs queryReadConcern = ReadConcernArgs();

        // Indicates if we want to skip the first document during oplog fetching or not.
        StartingPoint startingPoint = StartingPoint::kSkipFirstDoc;

        // Specifies if the oplog fetcher should request a resume token and provide it to
        // _enqueueDocumentsFn.
        bool requestResumeToken = false;

        std::string name = "oplog fetcher";

        // If true, the oplog fetcher will use an aggregation request with '$match' rather than
        // a 'find' query.
        bool forTenantMigration;
    };

    /**
     * Invariants if validation fails on any of the provided arguments.
     */
    OplogFetcher(executor::TaskExecutor* executor,
                 std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
                 DataReplicatorExternalState* dataReplicatorExternalState,
                 EnqueueDocumentsFn enqueueDocumentsFn,
                 OnShutdownCallbackFn onShutdownCallbackFn,
                 Config config);

    virtual ~OplogFetcher();

    /**
     * Validates documents in current batch of results returned from tailing the remote oplog.
     * 'first' should be set to true if this set of documents is the first batch returned from the
     * query.
     * On success, returns statistics on operations.
     */
    static StatusWith<DocumentsInfo> validateDocuments(
        const Documents& documents,
        bool first,
        Timestamp lastTS,
        StartingPoint startingPoint = StartingPoint::kSkipFirstDoc);

    /**
     * Allows the OplogFetcher to use an already-established connection from the caller.  Ownership
     * of the connection is taken by the OplogFetcher.  Must be called before startup.
     */
    void setConnection(std::unique_ptr<DBClientConnection>&& _connectedClient);

    /**
     * Prints out the status and settings of the oplog fetcher.
     */
    std::string toString();

    // ================== Test support API ===================

    /**
     * Returns the StartingPoint defined in the OplogFetcher::Config.
     */
    StartingPoint getStartingPoint_forTest() const;

    /**
     * Returns the `find` query run on the sync source's oplog.
     */
    FindCommandRequest makeFindCmdRequest_forTest(long long findTimeout) const;

    /**
     * Returns the OpTime of the last oplog entry fetched and processed.
     */
    OpTime getLastOpTimeFetched_forTest() const;

    /**
     * Returns the await data timeout used for the "maxTimeMS" field in getMore command requests.
     */
    Milliseconds getAwaitDataTimeout_forTest() const;

    /**
     * Type of function to create a database client connection. Used for testing only.
     */
    using CreateClientFn = std::function<std::unique_ptr<DBClientConnection>()>;

    /**
     * Overrides how the OplogFetcher creates the client. Used for testing only.
     */
    void setCreateClientFn_forTest(const CreateClientFn& createClientFn);

    /**
     * Get a raw pointer to the client connection. It is the caller's responsibility to not reuse
     * this pointer beyond the lifetime of the underlying client. Used for testing only.
     */
    DBClientConnection* getDBClientConnection_forTest() const;

    /**
     * Returns how long the `find` command should wait before timing out.
     */
    Milliseconds getInitialFindMaxTime_forTest() const;

    /**
     * Returns how long the `find` command should wait before timing out, if we are retrying the
     * `find` due to an error.
     */
    Milliseconds getRetriedFindMaxTime_forTest() const;

protected:
    /**
     * Returns the OpTime of the last oplog entry fetched and processed.
     */
    virtual OpTime _getLastOpTimeFetched() const;

private:
    // =============== AbstractAsyncComponent overrides ================

    /**
     * Schedules the _runQuery function to run in a separate thread.
     */
    void _doStartup_inlock() override;

    /**
     * Shuts down the DBClientCursor and DBClientConnection. Uses the connection's
     * shutdownAndDisallowReconnect function to interrupt it.
     */
    void _doShutdown_inlock() noexcept override;

    void _preJoin() noexcept override {}

    Mutex* _getMutex() noexcept override;

    // ============= End AbstractAsyncComponent overrides ==============

    /**
     * Creates a DBClientConnection and executes a query to retrieve oplog entries from this node's
     * sync source. This will create a tailable, awaitData, exhaust cursor which will be used until
     * the cursor fails or OplogFetcher is shut down. For each batch returned by the upstream node,
     * _onSuccessfulBatch will be called with the response.
     *
     * In the case of any network or response errors, this method will close the cursor and restart
     * a new one. If OplogFetcherRestartDecision's shouldContinue function indicates it should not
     * create a new cursor, it will call _finishCallback.
     */
    void _runQuery(const executor::TaskExecutor::CallbackArgs& callbackData) noexcept;

    /**
     * Establishes the initial connection to the sync source and authenticates the connection for
     * replication. This will also retry on connection failures until it exhausts the allowed retry
     * attempts.
     */
    Status _connect();

    /**
     * Sets the RequestMetadataWriter and ReplyMetadataReader on the connection.
     */
    void _setMetadataWriterAndReader();

    /**
     * Does one of:
     * 1. Executes a `find` query on the sync source's oplog and establishes a tailable, awaitData,
     * exhaust cursor.
     * 2. Executes a 'aggregate' query on the sync source's oplog. This is currently used in
     * tenant migrations.
     *
     * Before running the query, it will set a RequestMetadataWriter to modify the request to
     * include $oplogQueryData and $replData. If will also set a ReplyMetadataReader to parse the
     * response for the metadata field.
     */
    Status _createNewCursor(bool initialFind);

    /**
     * This function will create an `AggregateCommandRequest` object that will do a `$match` to find
     * all entries greater than the last fetched timestamp.
     */
    AggregateCommandRequest _makeAggregateCommandRequest(long long maxTimeMs,
                                                         Timestamp startTs) const;

    /**
     * This function will create the `find` query to issue to the sync source. It is provided with
     * the value to use as the "maxTimeMS" for the find command.
     */
    FindCommandRequest _makeFindCmdRequest(long long findTimeout) const;

    /**
     * Gets the next batch from the exhaust cursor.
     *
     * If there was an error getting the next batch, checks _oplogFetcherRestartDecision's
     * shouldContinue function to see if it should create a new cursor and if so, calls
     * _createNewCursor.
     */
    StatusWith<Documents> _getNextBatch();

    /**
     * Function called by the oplog fetcher when it gets a successful batch from the sync source.
     * This will also process the metadata received from the response.
     *
     * On failure returns a status that will be passed to _finishCallback.
     */
    Status _onSuccessfulBatch(const Documents& documents);

    /**
     * Notifies caller that the oplog fetcher has completed processing operations from the remote
     * oplog using the "_onShutdownCallbackFn".
     */
    void _finishCallback(Status status);

    /**
     * Sets the socket timeout on the connection to the source node. It will add a network buffer to
     * the provided timeout.
     */
    void _setSocketTimeout(long long timeout);

    /**
     * Returns how long the `find` command should wait before timing out.
     */
    Milliseconds _getInitialFindMaxTime() const;

    /**
     * Returns how long the `find` command should wait before timing out, if we are retrying the
     * `find` due to an error. This timeout should be considerably smaller than our initial oplog
     * `find` time, since a communication failure with an upstream node may indicate it is
     * unreachable.
     */
    Milliseconds _getRetriedFindMaxTime() const;

    /**
     * Checks the first batch of results from query.
     * 'documents' are the first batch of results returned from tailing the remote oplog.
     * 'remoteLastOpApplied' is the last OpTime applied on the sync source.
     * 'remoteRBID' is a RollbackId for the sync source returned in this oplog query.
     *
     * Returns TooStaleToSyncFromSource if we are too stale to sync from our source.
     * Returns OplogStartMissing if we should go into rollback.
     */
    Status _checkRemoteOplogStart(const OplogFetcher::Documents& documents,
                                  OpTime remoteLastOpApplied,
                                  int remoteRBID);

    /**
     * Distinguishes between needing to rollback and being too stale to sync from our sync source.
     * This will be called when we check the first batch of results and our last fetched optime does
     * not equal the first document in that batch. This function should never return Status::OK().
     */
    Status _checkTooStaleToSyncFromSource(OpTime lastFetched, OpTime firstOpTimeInBatch);

    // Protects member data of this OplogFetcher.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogFetcher::_mutex");

    // Namespace of the oplog to read.
    const NamespaceString _nss = NamespaceString::kRsOplogNamespace;

    // Rollback ID that the sync source had after the first batch. Initialized from
    // the requiredRBID in the OplogFetcher::Config and passed to the onShutdown callback.
    int _receivedRBID;

    // Indicates whether the current batch is the first received via this cursor.
    bool _firstBatch = true;

    // In the case of an error, this will help decide if a new cursor should be created or the
    // oplog fetcher should be shut down.
    std::unique_ptr<OplogFetcherRestartDecision> _oplogFetcherRestartDecision;

    // Function to call when the oplog fetcher shuts down.
    OnShutdownCallbackFn _onShutdownCallbackFn;

    // Used to keep track of the last oplog entry read and processed from the sync source.
    OpTime _lastFetched;

    // Logical time metadata handling hook for the DBClientConnection.
    std::unique_ptr<rpc::VectorClockMetadataHook> _vectorClockMetadataHook;

    // Set by the ReplyMetadataReader upon receiving a new batch.
    BSONObj _metadataObj;

    // Connection to the sync source whose oplog we will be querying. This connection should be
    // created with autoreconnect set to true so that it will automatically reconnect on a
    // connection failure. When the OplogFetcher is shut down, the connection will be interrupted
    // via its shutdownAndDisallowReconnect function.
    std::unique_ptr<DBClientConnection> _conn;

    // Used to create the DBClientConnection for the oplog fetcher.
    CreateClientFn _createClientFn;

    // The tailable, awaitData, exhaust cursor used to fetch oplog entries from the sync source.
    // When an error is encountered, depending on the result of OplogFetcherRestartDecision's
    // shouldContinue function, a new cursor will be created or the oplog fetcher will shut down.
    std::unique_ptr<DBClientCursor> _cursor;

    DataReplicatorExternalState* const _dataReplicatorExternalState;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    const Milliseconds _awaitDataTimeout;
    Config _config;

    // Handle to currently scheduled _runQuery task.
    executor::TaskExecutor::CallbackHandle _runQueryHandle;

    int _lastBatchElapsedMS = 0;

    // Condition to be notified on shutdown.
    stdx::condition_variable _shutdownCondVar;
};

class OplogFetcherFactory {
public:
    virtual ~OplogFetcherFactory() = default;
    virtual std::unique_ptr<OplogFetcher> operator()(
        executor::TaskExecutor* executor,
        std::unique_ptr<OplogFetcher::OplogFetcherRestartDecision> oplogFetcherRestartDecision,
        DataReplicatorExternalState* dataReplicatorExternalState,
        OplogFetcher::EnqueueDocumentsFn enqueueDocumentsFn,
        OplogFetcher::OnShutdownCallbackFn onShutdownCallbackFn,
        OplogFetcher::Config config) const = 0;
};

template <class T>
class OplogFetcherFactoryImpl : public OplogFetcherFactory {
public:
    std::unique_ptr<OplogFetcher> operator()(
        executor::TaskExecutor* executor,
        std::unique_ptr<OplogFetcher::OplogFetcherRestartDecision> oplogFetcherRestartDecision,
        DataReplicatorExternalState* dataReplicatorExternalState,
        OplogFetcher::EnqueueDocumentsFn enqueueDocumentsFn,
        OplogFetcher::OnShutdownCallbackFn onShutdownCallbackFn,
        OplogFetcher::Config config) const final {
        return std::make_unique<T>(executor,
                                   std::move(oplogFetcherRestartDecision),
                                   dataReplicatorExternalState,
                                   std::move(enqueueDocumentsFn),
                                   std::move(onShutdownCallbackFn),
                                   std::move(config));
    }

    static std::unique_ptr<OplogFetcherFactory> get() {
        return std::make_unique<OplogFetcherFactoryImpl<T>>();
    }
};

typedef OplogFetcherFactoryImpl<OplogFetcher> CreateOplogFetcherFn;

}  // namespace repl
}  // namespace mongo
