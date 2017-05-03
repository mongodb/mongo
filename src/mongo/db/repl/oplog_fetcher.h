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

#include <cstddef>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/abstract_oplog_fetcher.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace repl {

MONGO_FP_FORWARD_DECLARE(stopReplProducer);

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
 *
 * This class subclasses AbstractOplogFetcher which takes care of scheduling the Fetcher and
 * `getMore` commands, and handles restarting on errors.
 */
class OplogFetcher : public AbstractOplogFetcher {
    MONGO_DISALLOW_COPYING(OplogFetcher);

public:
    static Seconds kDefaultProtocolZeroAwaitDataTimeout;

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
     * struct.
     */
    using EnqueueDocumentsFn = stdx::function<Status(Fetcher::Documents::const_iterator begin,
                                                     Fetcher::Documents::const_iterator end,
                                                     const DocumentsInfo& info)>;

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
     * Invariants if validation fails on any of the provided arguments.
     */
    OplogFetcher(executor::TaskExecutor* executor,
                 OpTimeWithHash lastFetched,
                 HostAndPort source,
                 NamespaceString nss,
                 ReplSetConfig config,
                 std::size_t maxFetcherRestarts,
                 int requiredRBID,
                 bool requireFresherSyncSource,
                 DataReplicatorExternalState* dataReplicatorExternalState,
                 EnqueueDocumentsFn enqueueDocumentsFn,
                 OnShutdownCallbackFn onShutdownCallbackFn);

    virtual ~OplogFetcher();

    // ================== Test support API ===================

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
    BSONObj _makeFindCommandObject(const NamespaceString& nss,
                                   OpTime lastOpTimeFetched) const override;

    BSONObj _makeMetadataObject() const override;

    /**
     * This function is run by the AbstractOplogFetcher on a successful batch of oplog entries.
     */
    StatusWith<BSONObj> _onSuccessfulBatch(const Fetcher::QueryResponse& queryResponse) override;

    // The metadata object sent with the Fetcher queries.
    const BSONObj _metadataObject;

    // Rollback ID that the sync source is required to have after the first batch.
    int _requiredRBID;

    // A boolean indicating whether we should error if the sync source is not ahead of our initial
    // last fetched OpTime on the first batch. Most of the time this should be set to true,
    // but there are certain special cases, namely during initial sync, where it's acceptable for
    // our sync source to have no ops newer than _lastFetched.
    bool _requireFresherSyncSource;

    DataReplicatorExternalState* const _dataReplicatorExternalState;
    const EnqueueDocumentsFn _enqueueDocumentsFn;
    const Milliseconds _awaitDataTimeout;
};

}  // namespace repl
}  // namespace mongo
