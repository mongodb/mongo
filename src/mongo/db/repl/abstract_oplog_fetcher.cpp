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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/abstract_oplog_fetcher.h"

#include "mongo/base/counter.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_parameters.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

Counter64 readersCreatedStats;
ServerStatusMetricField<Counter64> displayReadersCreated("repl.network.readersCreated",
                                                         &readersCreatedStats);

// Number of seconds for the `maxTimeMS` on the initial `find` command.
//
// For the initial 'find' request, we provide a generous timeout, to account for the potentially
// slow process of a sync source finding the lastApplied optime provided in a node's query in its
// oplog.
MONGO_EXPORT_SERVER_PARAMETER(oplogInitialFindMaxSeconds, int, 60);

// Number of seconds for the `maxTimeMS` on any retried `find` commands.
MONGO_EXPORT_SERVER_PARAMETER(oplogRetriedFindMaxSeconds, int, 2);

// Number of milliseconds to add to the `find` and `getMore` timeouts to calculate the network
// timeout for the requests.
const Milliseconds kNetworkTimeoutBufferMS{5000};

// Default `maxTimeMS` timeout for `getMore`s.
const Milliseconds kDefaultOplogGetMoreMaxMS{5000};


}  // namespace

StatusWith<OpTimeWithHash> AbstractOplogFetcher::parseOpTimeWithHash(const BSONObj& oplogEntryObj) {
    const auto opTime = OpTime::parseFromOplogEntry(oplogEntryObj);
    if (!opTime.isOK()) {
        return opTime.getStatus();
    }

    long long hash = 0;
    Status hashStatus = bsonExtractIntegerField(oplogEntryObj, "h"_sd, &hash);
    if (!hashStatus.isOK()) {
        return hashStatus;
    }

    return OpTimeWithHash{hash, opTime.getValue()};
}

AbstractOplogFetcher::AbstractOplogFetcher(executor::TaskExecutor* executor,
                                           OpTimeWithHash lastFetched,
                                           HostAndPort source,
                                           NamespaceString nss,
                                           std::size_t maxFetcherRestarts,
                                           OnShutdownCallbackFn onShutdownCallbackFn,
                                           const std::string& componentName)
    : AbstractAsyncComponent(executor, componentName),
      _source(source),
      _nss(nss),
      _maxFetcherRestarts(maxFetcherRestarts),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(lastFetched) {

    invariant(!_lastFetched.opTime.isNull());
    invariant(onShutdownCallbackFn);
}

Milliseconds AbstractOplogFetcher::_getInitialFindMaxTime() const {
    return Milliseconds(oplogInitialFindMaxSeconds.load() * 1000);
}

Milliseconds AbstractOplogFetcher::_getRetriedFindMaxTime() const {
    return Milliseconds(oplogRetriedFindMaxSeconds.load() * 1000);
}

Milliseconds AbstractOplogFetcher::_getGetMoreMaxTime() const {
    return kDefaultOplogGetMoreMaxMS;
}

std::string AbstractOplogFetcher::toString() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    str::stream msg;
    msg << _getComponentName() << " -"
        << " last optime fetched: " << _lastFetched.opTime.toString()
        << " last hash fetched: " << _lastFetched.value;
    // The fetcher is created a startup, not at construction, so we must check if it exists.
    if (_fetcher) {
        msg << " fetcher: " << _fetcher->getDiagnosticString();
    }
    return msg;
}

void AbstractOplogFetcher::_makeAndScheduleFetcherCallback(
    const executor::TaskExecutor::CallbackArgs& args) {
    Status responseStatus = _checkForShutdownAndConvertStatus(args, "error scheduling fetcher");
    if (!responseStatus.isOK()) {
        _finishCallback(responseStatus);
        return;
    }

    BSONObj findCommandObj = _makeFindCommandObject(
        _nss, _getLastOpTimeWithHashFetched().opTime, _getInitialFindMaxTime());
    BSONObj metadataObj = _makeMetadataObject();

    Status scheduleStatus = Status::OK();
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _fetcher = _makeFetcher(findCommandObj, metadataObj, _getInitialFindMaxTime());
        scheduleStatus = _scheduleFetcher_inlock();
    }
    if (!scheduleStatus.isOK()) {
        _finishCallback(scheduleStatus);
        return;
    }
}

Status AbstractOplogFetcher::_doStartup_inlock() noexcept {
    return _scheduleWorkAndSaveHandle_inlock(
        stdx::bind(
            &AbstractOplogFetcher::_makeAndScheduleFetcherCallback, this, stdx::placeholders::_1),
        &_makeAndScheduleFetcherHandle,
        "_makeAndScheduleFetcherCallback");
}

void AbstractOplogFetcher::_doShutdown_inlock() noexcept {
    _cancelHandle_inlock(_makeAndScheduleFetcherHandle);
    if (_fetcher) {
        _fetcher->shutdown();
    }
}

stdx::mutex* AbstractOplogFetcher::_getMutex() noexcept {
    return &_mutex;
}

Status AbstractOplogFetcher::_scheduleFetcher_inlock() {
    readersCreatedStats.increment();
    return _fetcher->schedule();
}

OpTimeWithHash AbstractOplogFetcher::getLastOpTimeWithHashFetched_forTest() const {
    return _getLastOpTimeWithHashFetched();
}

OpTimeWithHash AbstractOplogFetcher::_getLastOpTimeWithHashFetched() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _lastFetched;
}

BSONObj AbstractOplogFetcher::getCommandObject_forTest() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _fetcher->getCommandObject();
}

BSONObj AbstractOplogFetcher::getFindQuery_forTest() const {
    return _makeFindCommandObject(
        _nss, _getLastOpTimeWithHashFetched().opTime, _getInitialFindMaxTime());
}

HostAndPort AbstractOplogFetcher::_getSource() const {
    return _source;
}

NamespaceString AbstractOplogFetcher::_getNamespace() const {
    return _nss;
}

void AbstractOplogFetcher::_callback(const Fetcher::QueryResponseStatus& result,
                                     BSONObjBuilder* getMoreBob) {
    Status responseStatus =
        _checkForShutdownAndConvertStatus(result.getStatus(), "error in fetcher batch callback");
    if (ErrorCodes::CallbackCanceled == responseStatus) {
        LOG(1) << _getComponentName() << " oplog query cancelled to " << _getSource() << ": "
               << redact(responseStatus);
        _finishCallback(responseStatus);
        return;
    }

    // If target cut connections between connecting and querying (for
    // example, because it stepped down) we might not have a cursor.
    if (!responseStatus.isOK()) {

        BSONObj findCommandObj = _makeFindCommandObject(
            _nss, _getLastOpTimeWithHashFetched().opTime, _getRetriedFindMaxTime());
        BSONObj metadataObj = _makeMetadataObject();
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_fetcherRestarts == _maxFetcherRestarts) {
                log() << "Error returned from oplog query (no more query restarts left): "
                      << redact(responseStatus);
            } else {
                log() << "Restarting oplog query due to error: " << redact(responseStatus)
                      << ". Last fetched optime (with hash): " << _lastFetched
                      << ". Restarts remaining: " << (_maxFetcherRestarts - _fetcherRestarts);
                _fetcherRestarts++;
                // Destroying current instance in _shuttingDownFetcher will possibly block.
                _shuttingDownFetcher.reset();
                // Move the old fetcher into the shutting down instance.
                _shuttingDownFetcher.swap(_fetcher);
                // Create and start fetcher with current term and new starting optime, and use the
                // retry 'find' timeout.
                _fetcher = _makeFetcher(findCommandObj, metadataObj, _getRetriedFindMaxTime());

                auto scheduleStatus = _scheduleFetcher_inlock();
                if (scheduleStatus.isOK()) {
                    log() << "Scheduled new oplog query " << _fetcher->toString();
                    return;
                }
                error() << "Error scheduling new oplog query: " << redact(scheduleStatus)
                        << ". Returning current oplog query error: " << redact(responseStatus);
            }
        }
        _finishCallback(responseStatus);
        return;
    }

    // Reset fetcher restart counter on successful response.
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_isActive_inlock());
        _fetcherRestarts = 0;
    }

    if (_isShuttingDown()) {
        _finishCallback(
            Status(ErrorCodes::CallbackCanceled, _getComponentName() + " shutting down"));
        return;
    }

    // At this point we have a successful batch and can call the subclass's _onSuccessfulBatch.
    const auto& queryResponse = result.getValue();
    auto batchResult = _onSuccessfulBatch(queryResponse);
    if (!batchResult.isOK()) {
        // The stopReplProducer fail point expects this to return successfully. If another fail
        // point wants this to return unsuccessfully, it should use a different error code.
        if (batchResult.getStatus() == ErrorCodes::FailPointEnabled) {
            _finishCallback(Status::OK());
            return;
        }
        _finishCallback(batchResult.getStatus());
        return;
    }

    // No more data. Stop processing and return Status::OK.
    if (!getMoreBob) {
        _finishCallback(Status::OK());
        return;
    }

    // We have now processed the batch and should move forward our view of _lastFetched. Note that
    // the _lastFetched value will not be updated until the _onSuccessfulBatch function is
    // completed.
    const auto& documents = queryResponse.documents;
    if (documents.size() > 0) {
        auto lastDocRes = AbstractOplogFetcher::parseOpTimeWithHash(documents.back());
        if (!lastDocRes.isOK()) {
            _finishCallback(lastDocRes.getStatus());
            return;
        }
        auto lastDoc = lastDocRes.getValue();
        LOG(3) << _getComponentName()
               << " setting last fetched optime ahead after batch: " << lastDoc.opTime
               << "; hash: " << lastDoc.value;

        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _lastFetched = lastDoc;
    }

    // Check for shutdown to save an unnecessary `getMore` request.
    if (_isShuttingDown()) {
        _finishCallback(
            Status(ErrorCodes::CallbackCanceled, _getComponentName() + " shutting down"));
        return;
    }

    // The _onSuccessfulBatch function returns the `getMore` command we want to send.
    getMoreBob->appendElements(batchResult.getValue());
}

void AbstractOplogFetcher::_finishCallback(Status status) {
    invariant(isActive());

    _onShutdownCallbackFn(status);

    decltype(_onShutdownCallbackFn) onShutdownCallbackFn;
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _transitionToComplete_inlock();

    // Release any resources that might be held by the '_onShutdownCallbackFn' function object.
    // The function object will be destroyed outside the lock since the temporary variable
    // 'onShutdownCallbackFn' is declared before 'lock'.
    invariant(_onShutdownCallbackFn);
    std::swap(_onShutdownCallbackFn, onShutdownCallbackFn);
}

std::unique_ptr<Fetcher> AbstractOplogFetcher::_makeFetcher(const BSONObj& findCommandObj,
                                                            const BSONObj& metadataObj,
                                                            Milliseconds findMaxTime) {
    return stdx::make_unique<Fetcher>(
        _getExecutor(),
        _source,
        _nss.db().toString(),
        findCommandObj,
        stdx::bind(
            &AbstractOplogFetcher::_callback, this, stdx::placeholders::_1, stdx::placeholders::_3),
        metadataObj,
        findMaxTime + kNetworkTimeoutBufferMS,
        _getGetMoreMaxTime() + kNetworkTimeoutBufferMS);
}

}  // namespace repl
}  // namespace mongo
