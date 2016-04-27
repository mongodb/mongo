/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

/**
 * Used to keep track of the optime and hash of the last fetched operation.
 */
using OpTimeWithHash = OpTimeWith<long long>;

/**
 * The oplog fetcher, once started, reads operations from a remote oplog using a tailable cursor.
 *
 * The initial find command is generated from last fetched optime and hash and may contain the
 * current term depending on the replica set config provided.
 *
 * Forwards metadata in each find/getMore response to the data replicator external state.
 *
 * Performs additional validation on first batch of operations returned from the query to ensure we
 * are able to continue from our last known fetched operation.
 *
 * Validates each batch of operations.
 *
 * Pushes operations from each batch of operations onto a buffer using the "enqueueDocumentsFn"
 * function.
 *
 * Issues a getMore command after successfully processing each batch of operations.
 *
 * When there is an error or when it is not possible to issue another getMore request, calls
 * "onShutdownCallbackFn" to signal the end of processing.
 */
class OplogFetcher {
    MONGO_DISALLOW_COPYING(OplogFetcher);

public:
    static Seconds kDefaultProtocolZeroAwaitDataTimeout;

    /**
     * Type of function called by the oplog fetcher on shutdown with
     * the final oplog fetcher status, last optime fetched and last hash fetched.
     *
     * The status will be Status::OK() if we have processed the last batch of operations
     * from the tailable cursor ("bob" is null in the fetcher callback).
     */
    using OnShutdownCallbackFn =
        stdx::function<void(const Status& shutdownStatus, const OpTimeWithHash& lastFetched)>;

    /**
     * Statistics on current batch of operations returned by the fetcher.
     */
    struct DocumentsInfo {
        size_t networkDocumentCount = 0;
        size_t networkDocumentBytes = 0;
        size_t toApplyDocumentCount = 0;
        size_t toApplyDocumentBytes = 0;
        OpTimeWithHash lastDocument = {0, OpTime()};
    };

    /**
     * Type of function that accepts a pair of iterators into a range of operations
     * within the current batch of results and copies the operations into
     * a buffer to be consumed by the next stage of the replication process.
     *
     * Additional information on the operations is provided in a DocumentsInfo
     * struct and duration for how long the last remote command took to complete.
     */
    using EnqueueDocumentsFn = stdx::function<void(Fetcher::Documents::const_iterator begin,
                                                   Fetcher::Documents::const_iterator end,
                                                   const DocumentsInfo& info,
                                                   Milliseconds remoteCommandProcessingTime)>;

    /**
     * Validates documents in current batch of results returned from tailing the remote oplog.
     * 'first' should be set to true if this set of documents is the first batch returned from the
     * query.
     * On success, returns statistics on operations.
     */
    static StatusWith<DocumentsInfo> validateDocuments(const Fetcher::Documents& documents,
                                                       bool first,
                                                       Timestamp lastTS);

    /**
     * Initializes fetcher with command to tail remote oplog.
     *
     * Throws a UserException if validation fails on any of the provided arguments.
     */
    OplogFetcher(executor::TaskExecutor* exec,
                 OpTimeWithHash lastFetched,
                 HostAndPort source,
                 NamespaceString nss,
                 ReplicaSetConfig config,
                 DataReplicatorExternalState* dataReplicatorExternalState,
                 EnqueueDocumentsFn enqueueDocumentsFn,
                 OnShutdownCallbackFn onShutdownCallbackFn);

    virtual ~OplogFetcher() = default;

    std::string toString() const;

    /**
     * Returns true if we have scheduled the fetcher to read the oplog on the sync source.
     */
    bool isActive() const;

    /**
     * Starts fetcher so that we begin tailing the remote oplog on the sync source.
     */
    Status startup();

    /**
     * Cancels both scheduled and active remote command requests.
     * Returns immediately if the Oplog Fetcher is not active.
     * It is fine to call this multiple times.
     */
    void shutdown();

    /**
     * Waits until the oplog fetcher is inactive.
     * It is fine to call this multiple times.
     */
    void join();

    /**
     * Returns optime and hash of the last oplog entry in the most recent oplog query result.
     */
    OpTimeWithHash getLastOpTimeWithHashFetched() const;

    // ================== Test support API ===================

    /**
     * Returns command object sent in first remote command.
     */
    BSONObj getCommandObject_forTest() const;

    /**
     * Returns metadata object sent in remote commands.
     */
    BSONObj getMetadataObject_forTest() const;

    /**
     * Returns timeout for remote commands to complete.
     */
    Milliseconds getRemoteCommandTimeout_forTest() const;

    /**
     * Returns the await data timeout used for the "maxTimeMS" field in getMore command requests.
     */
    Milliseconds getAwaitDataTimeout_forTest() const;

private:
    /**
     * Processes each batch of results from the tailable cursor started by the fetcher on the sync
     * source.
     *
     * Calls "onShutdownCallbackFn" if there is an error or if there are no further results to
     * request from the sync source.
     */
    void _callback(const Fetcher::QueryResponseStatus& result, BSONObjBuilder* getMoreBob);

    /**
     * Notifies caller that the oplog fetcher has completed processing operations from
     * the remote oplog.
     */
    void _onShutdown(Status status);
    void _onShutdown(Status status, OpTimeWithHash opTimeWithHash);

    DataReplicatorExternalState* _dataReplicatorExternalState;
    Fetcher _fetcher;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    const Milliseconds _awaitDataTimeout;
    const OnShutdownCallbackFn _onShutdownCallbackFn;

    // Protects member data of this Fetcher.
    mutable stdx::mutex _mutex;

    // Used to validate start of first batch of results from the remote oplog
    // tailing query and to keep track of the last known operation consumed via
    // "_enqueueDocumentsFn".
    OpTimeWithHash _lastFetched;
};

}  // namespace repl
}  // namespace mongo
