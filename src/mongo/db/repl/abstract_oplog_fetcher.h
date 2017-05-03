/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/abstract_async_component.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace repl {

/**
 * Used to keep track of the OpTime and hash of the last fetched operation.
 */
using OpTimeWithHash = OpTimeWith<long long>;

/**
 * This class represents an abstract base class for replication components that try to read from
 * remote oplogs. An abstract oplog fetcher is an abstract async component. It owns a Fetcher
 * that fetches operations from a remote oplog and restarts from the last fetched oplog entry on
 * error.
 *
 * The `find` command and metadata are provided by oplog fetchers that subclass the abstract oplog
 * fetcher. Subclasses also provide a callback to run on successful batches.
 */
class AbstractOplogFetcher : public AbstractAsyncComponent {
    MONGO_DISALLOW_COPYING(AbstractOplogFetcher);

public:
    /**
     * Type of function called by the abstract oplog fetcher on shutdown with
     * the final abstract oplog fetcher status.
     *
     * The status will be Status::OK() if we have processed the last batch of operations
     * from the cursor ("bob" is null in the fetcher callback).
     *
     * This function will be called 0 times if startup() fails and at most once after startup()
     * returns success.
     */
    using OnShutdownCallbackFn = stdx::function<void(const Status& shutdownStatus)>;

    /**
     * Constants used to specify how long the `find` command, `getMore` commands, and network
     * operations should wait before timing out.
     */
    static const Seconds kOplogInitialFindMaxTime;
    static const Seconds kOplogGetMoreMaxTime;
    static const Seconds kOplogQueryNetworkTimeout;

    /**
     * This function takes a BSONObj oplog entry and parses out the OpTime and hash.
     */
    static StatusWith<OpTimeWithHash> parseOpTimeWithHash(const BSONObj& oplogEntryObj);

    /**
     * Invariants if validation fails on any of the provided arguments.
     */
    AbstractOplogFetcher(executor::TaskExecutor* executor,
                         OpTimeWithHash lastFetched,
                         HostAndPort source,
                         NamespaceString nss,
                         std::size_t maxFetcherRestarts,
                         OnShutdownCallbackFn onShutdownCallbackFn,
                         const std::string& componentName);

    virtual ~AbstractOplogFetcher() = default;

    std::string toString() const;

    // ================== Test support API ===================

    /**
     * Returns the command object sent in first remote command. Since the Fetcher is not created
     * until startup, this cannot be used until the Fetcher is guaranteed to exist.
     */
    BSONObj getCommandObject_forTest() const;

    /**
     * Returns the `find` query provided to the Fetcher. Since the Fetcher is not created until
     * startup, this can be used for logging the `find` query before startup.
     */
    BSONObj getFindQuery_forTest() const;

    /**
     * Returns the OpTime and hash of the last oplog entry fetched and processed.
     */
    OpTimeWithHash getLastOpTimeWithHashFetched_forTest() const;

protected:
    /**
     * Returns the sync source from which this oplog fetcher is fetching.
     */
    HostAndPort _getSource() const;

    /**
     * Returns the namespace from which this oplog fetcher is fetching.
     */
    NamespaceString _getNamespace() const;

    /**
     * Returns the OpTime and hash of the last oplog entry fetched and processed.
     */
    OpTimeWithHash _getLastOpTimeWithHashFetched() const;

    // =============== AbstractAsyncComponent overrides ================

    /**
     * Initializes and schedules a Fetcher with a `find` command specified by the subclass.
     */
    virtual Status _doStartup_inlock() noexcept override;

    /**
     * Shuts down the Fetcher.
     */
    virtual void _doShutdown_inlock() noexcept override;

private:
    stdx::mutex* _getMutex() noexcept override;

    /**
     * This function must be overriden by subclass oplog fetchers to specify what `find` command
     * to issue to the sync source. The subclass is provided with the last OpTime fetched so that
     * it can begin its Fetcher from the middle of the oplog.
     */
    virtual BSONObj _makeFindCommandObject(const NamespaceString& nss,
                                           OpTime lastOpTimeFetched) const = 0;

    /**
     * This function must be overriden by subclass oplog fetchers to specify what metadata object
     * to send with the `find` command.
     */
    virtual BSONObj _makeMetadataObject() const = 0;

    /**
     * Function called by the abstract oplog fetcher when it gets a successful batch from
     * the sync source.
     *
     * On success, returns the BSONObj of the `getMore` command that should be sent back to the
     * sync source. On failure returns a status that will be passed to the _finishCallback.
     */
    virtual StatusWith<BSONObj> _onSuccessfulBatch(const Fetcher::QueryResponse& queryResponse) = 0;

    /**
     * This function creates a Fetcher with the given `find` command and metadata.
     */
    std::unique_ptr<Fetcher> _makeFetcher(const BSONObj& findCommandObj,
                                          const BSONObj& metadataObj);
    /**
     * Callback used to make a Fetcher, and then save and schedule it in a lock.
     */
    void _makeAndScheduleFetcherCallback(const executor::TaskExecutor::CallbackArgs& args);

    /**
     * Schedules fetcher and updates counters.
     */
    Status _scheduleFetcher_inlock();

    /**
     * Processes each batch of results from the cursor started by the Fetcher on the sync source.
     *
     * Calls "_finishCallback" if there is an error or if there are no further results to
     * request from the sync source.
     */
    void _callback(const Fetcher::QueryResponseStatus& result, BSONObjBuilder* getMoreBob);

    /**
     * Notifies caller that the oplog fetcher has completed processing operations from
     * the remote oplog using the "_onShutdownCallbackFn".
     */
    void _finishCallback(Status status);

    // Sync source to read from.
    const HostAndPort _source;

    // Namespace of the oplog to read.
    const NamespaceString _nss;

    // Maximum number of times to consecutively restart the Fetcher on non-cancellation errors.
    const std::size_t _maxFetcherRestarts;

    // Protects member data of this AbstractOplogFetcher.
    mutable stdx::mutex _mutex;

    // Function to call when the oplog fetcher shuts down.
    OnShutdownCallbackFn _onShutdownCallbackFn;

    // Used to keep track of the last oplog entry read and processed from the sync source.
    OpTimeWithHash _lastFetched;

    // Fetcher restarts since the last successful oplog query response.
    std::size_t _fetcherRestarts = 0;

    std::unique_ptr<Fetcher> _fetcher;
    std::unique_ptr<Fetcher> _shuttingDownFetcher;

    // Handle to currently scheduled _makeAndScheduleFetcherCallback task.
    executor::TaskExecutor::CallbackHandle _makeAndScheduleFetcherHandle;
};

}  // namespace repl
}  // namespace mongo
