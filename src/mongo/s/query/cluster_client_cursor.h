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

#include <boost/optional.hpp>

#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;
template <typename T>
class StatusWith;

/**
 * ClusterClientCursor is used to generate results from cursor-generating commands on one or
 * more remote hosts. A cursor-generating command (e.g. the find command) is one that
 * establishes a ClientCursor and a matching cursor id on the remote host. In order to retrieve
 * all command results, getMores must be issued against each of the remote cursors until they
 * are exhausted.
 *
 * Results are generated using a pipeline of mongoS query execution stages called RouterExecStage.
 *
 * Does not throw exceptions.
 */
class ClusterClientCursor {
public:
    virtual ~ClusterClientCursor(){};

    /**
     * Returns the next available result document (along with an ok status). May block waiting
     * for results from remote nodes.
     *
     * If there are no further results, the end of the stream is indicated with an empty
     * QueryResult and an ok status.
     *
     * A non-ok status is returned in case of any error.
     */
    virtual StatusWith<ClusterQueryResult> next() = 0;

    /**
     * Must be called before destruction to abandon a not-yet-exhausted cursor. If next() has
     * already returned boost::none, then the cursor is exhausted and is safe to destroy.
     *
     * May block waiting for responses from remote hosts.
     */
    virtual void kill(OperationContext* opCtx) = 0;

    /**
     * Sets the operation context for the cursor.
     */
    virtual void reattachToOperationContext(OperationContext* opCtx) = 0;

    /**
     * Detaches the cursor from its current OperationContext. Must be called before the
     * OperationContext in use is deleted.
     */
    virtual void detachFromOperationContext() = 0;

    /**
     * Return the current context the cursor is attached to, if any.
     */
    virtual OperationContext* getCurrentOperationContext() const = 0;

    /**
     * Returns whether or not this cursor is tailable.
     */
    virtual bool isTailable() const = 0;

    /**
     * Returns whether or not this cursor is tailable and awaitData.
     */
    virtual bool isTailableAndAwaitData() const = 0;

    /**
     * Returns the original command object which created this cursor.
     */
    virtual BSONObj getOriginatingCommand() const = 0;

    /**
     * Returns the privileges required to run a getMore against this cursor. This is the same as the
     * set of privileges which would have been required to create the cursor in the first place.
     */
    virtual const PrivilegeVector& getOriginatingPrivileges() const& = 0;
    void getOriginatingPrivileges() && = delete;

    /**
     * Returns true if the cursor was opened with 'allowPartialResults:true' and results are not
     * available from one or more shards.
     */
    virtual bool partialResultsReturned() const = 0;

    /**
     * Returns the number of remote hosts involved in this operation.
     */
    virtual std::size_t getNumRemotes() const = 0;

    /**
     * Returns the current most-recent resume token for this cursor, or an empty object if this is
     * not a $changeStream cursor.
     */
    virtual BSONObj getPostBatchResumeToken() const = 0;

    /**
     * Returns the number of result documents returned so far by this cursor via the next() method.
     */
    virtual long long getNumReturnedSoFar() const = 0;

    /**
     * Stash the ClusterQueryResult so that it gets returned from the CCC on a later call to
     * next().
     *
     * Queued documents are returned in FIFO order. The queued results are exhausted before
     * generating further results from the underlying mongos query stages.
     *
     * 'obj' must be owned BSON.
     */
    virtual void queueResult(const ClusterQueryResult& result) = 0;

    /**
     * Returns whether or not all the remote cursors underlying this cursor have been exhausted.
     */
    virtual bool remotesExhausted() = 0;

    /**
     * Sets the maxTimeMS value that the cursor should forward with any internally issued getMore
     * requests.
     *
     * Returns a non-OK status if this cursor type does not support maxTimeMS on getMore (i.e. if
     * the cursor is not tailable + awaitData).
     */
    virtual Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) = 0;

    /**
     * Returns the logical session id for this cursor.
     */
    virtual boost::optional<LogicalSessionId> getLsid() const = 0;

    /**
     * Returns the transaction number for this cursor.
     */
    virtual boost::optional<TxnNumber> getTxnNumber() const = 0;

    /**
     * Returns the APIParameters for this cursor.
     */
    virtual APIParameters getAPIParameters() const = 0;

    /**
     * Returns the readPreference for this cursor.
     */
    virtual boost::optional<ReadPreferenceSetting> getReadPreference() const = 0;

    /**
     * Returns the readConcern for this cursor.
     */
    virtual boost::optional<repl::ReadConcernArgs> getReadConcern() const = 0;

    /**
     * Returns the creation date of the cursor.
     */
    virtual Date_t getCreatedDate() const = 0;

    /**
     * Returns the date the cursor was last used.
     */
    virtual Date_t getLastUseDate() const = 0;

    /**
     * Set the last use date to the provided time.
     */
    virtual void setLastUseDate(Date_t now) = 0;

    /**
     * Returns the queryHash of the query.
     */
    virtual boost::optional<uint32_t> getQueryHash() const = 0;

    /**
     * Returns the number of batches returned by this cursor.
     */
    std::uint64_t getNBatches() const {
        return _metrics.nBatches.value_or(0);
    }

    /**
     * Increment the number of batches returned so far by one.
     */
    void incNBatches() {
        _metrics.incrementNBatches();
    }

    void incrementCursorMetrics(OpDebug::AdditiveMetrics newMetrics) {
        _metrics.add(newMetrics);
    }

    //
    // maxTimeMS support.
    //

    /**
     * Returns the amount of time execution time available to this cursor. Only valid at the
     * beginning of a getMore request, and only really for use by the maxTime tracking code.
     *
     * Microseconds::max() == infinity, values less than 1 mean no time left.
     */
    Microseconds getLeftoverMaxTimeMicros() const {
        return _leftoverMaxTimeMicros;
    }

    /**
     * Sets the amount of execution time available to this cursor. This is only called when an
     * operation that uses a cursor is finishing, to update its remaining time.
     *
     * Microseconds::max() == infinity, values less than 1 mean no time left.
     */
    void setLeftoverMaxTimeMicros(Microseconds leftoverMaxTimeMicros) {
        _leftoverMaxTimeMicros = leftoverMaxTimeMicros;
    }

    /**
     * Returns true if operations with this cursor should be omitted from diagnostic sources such as
     * currentOp and the profiler.
     */
    virtual bool shouldOmitDiagnosticInformation() const = 0;

protected:
    // Metrics that are accumulated over the lifetime of the cursor, incremented with each getMore.
    // Useful for diagnostics like telemetry.
    OpDebug::AdditiveMetrics _metrics;

private:
    // Unused maxTime budget for this cursor.
    Microseconds _leftoverMaxTimeMicros = Microseconds::max();
};

}  // namespace mongo
