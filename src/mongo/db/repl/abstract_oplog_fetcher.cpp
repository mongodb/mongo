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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/abstract_oplog_fetcher.h"

#include <memory>

#include "mongo/base/counter.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

// This failpoint is shared with oplog_fetcher.
MONGO_FAIL_POINT_DEFINE(hangBeforeStartingOplogFetcher)

namespace {
Counter64 readersCreatedStats;
ServerStatusMetricField<Counter64> displayReadersCreated("repl.network.readersCreated",
                                                         &readersCreatedStats);

// Default `maxTimeMS` timeout for `getMore`s.
const Milliseconds kDefaultOplogGetMoreMaxMS{5000};

}  // namespace

AbstractOplogFetcher::AbstractOplogFetcher(executor::TaskExecutor* executor,
                                           OpTime lastFetched,
                                           HostAndPort source,
                                           NamespaceString nss,
                                           std::size_t maxFetcherRestarts,
                                           OnShutdownCallbackFn onShutdownCallbackFn,
                                           const std::string& componentName)
    : AbstractOplogFetcher(executor,
                           lastFetched,
                           source,
                           nss,
                           std::make_unique<OplogFetcherRestartDecisionDefault>(maxFetcherRestarts),
                           onShutdownCallbackFn,
                           componentName) {
    invariant(!_lastFetched.isNull());
    invariant(onShutdownCallbackFn);
}

AbstractOplogFetcher::AbstractOplogFetcher(
    executor::TaskExecutor* executor,
    OpTime lastFetched,
    HostAndPort source,
    NamespaceString nss,
    std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
    OnShutdownCallbackFn onShutdownCallbackFn,
    const std::string& componentName)
    : AbstractAsyncComponent(executor, componentName),
      _source(source),
      _nss(nss),
      _oplogFetcherRestartDecision(std::move(oplogFetcherRestartDecision)),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(lastFetched) {
    invariant(!_lastFetched.isNull());
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

Milliseconds AbstractOplogFetcher::_getNetworkTimeoutBuffer() const {
    return Milliseconds(oplogNetworkTimeoutBufferSeconds.load() * 1000);
}

std::string AbstractOplogFetcher::toString() const {
    stdx::lock_guard<Latch> lock(_mutex);
    str::stream msg;
    msg << _getComponentName() << " -"
        << " last optime fetched: " << _lastFetched.toString();
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

    BSONObj findCommandObj =
        _makeFindCommandObject(_nss, _getLastOpTimeFetched(), _getInitialFindMaxTime());
    BSONObj metadataObj = _makeMetadataObject();

    Status scheduleStatus = Status::OK();
    {
        stdx::lock_guard<Latch> lock(_mutex);
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
        [this](const executor::TaskExecutor::CallbackArgs& args) {
            // Tests use this failpoint to prevent the oplog fetcher from starting.  If those
            // tests fail and the oplog fetcher is canceled, we want to continue so we see
            // a test failure quickly instead of a test timeout eventually.
            while (hangBeforeStartingOplogFetcher.shouldFail() && !args.myHandle.isCanceled()) {
                sleepmillis(100);
            }
            _makeAndScheduleFetcherCallback(args);
        },
        &_makeAndScheduleFetcherHandle,
        "_makeAndScheduleFetcherCallback");
}

void AbstractOplogFetcher::_doShutdown_inlock() noexcept {
    _cancelHandle_inlock(_makeAndScheduleFetcherHandle);
    if (_fetcher) {
        _fetcher->shutdown();
    }
}

Mutex* AbstractOplogFetcher::_getMutex() noexcept {
    return &_mutex;
}

Status AbstractOplogFetcher::_scheduleFetcher_inlock() {
    readersCreatedStats.increment();
    return _fetcher->schedule();
}

OpTime AbstractOplogFetcher::getLastOpTimeFetched_forTest() const {
    return _getLastOpTimeFetched();
}

OpTime AbstractOplogFetcher::_getLastOpTimeFetched() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _lastFetched;
}

BSONObj AbstractOplogFetcher::getCommandObject_forTest() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _fetcher->getCommandObject();
}

BSONObj AbstractOplogFetcher::getFindQuery_forTest() const {
    return _makeFindCommandObject(_nss, _getLastOpTimeFetched(), _getInitialFindMaxTime());
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
        LOGV2_DEBUG(21032,
                    1,
                    "{getComponentName} oplog query cancelled to {getSource}: {responseStatus}",
                    "getComponentName"_attr = _getComponentName(),
                    "getSource"_attr = _getSource(),
                    "responseStatus"_attr = redact(responseStatus));
        _finishCallback(responseStatus);
        return;
    }
    // If target cut connections between connecting and querying (for
    // example, because it stepped down) we might not have a cursor.
    if (!responseStatus.isOK()) {
        BSONObj findCommandObj =
            _makeFindCommandObject(_nss, _getLastOpTimeFetched(), _getRetriedFindMaxTime());
        BSONObj metadataObj = _makeMetadataObject();
        {
            if (_oplogFetcherRestartDecision->shouldContinue(this, responseStatus)) {
                stdx::lock_guard<Latch> lock(_mutex);
                // Destroying current instance in _shuttingDownFetcher will possibly block.
                _shuttingDownFetcher.reset();
                // Move the old fetcher into the shutting down instance.
                _shuttingDownFetcher.swap(_fetcher);
                // Create and start fetcher with current term and new starting optime, and use the
                // retry 'find' timeout.
                _fetcher = _makeFetcher(findCommandObj, metadataObj, _getRetriedFindMaxTime());

                auto scheduleStatus = _scheduleFetcher_inlock();
                if (scheduleStatus.isOK()) {
                    LOGV2(21033,
                          "Scheduled new oplog query {fetcher}",
                          "fetcher"_attr = _fetcher->toString());
                    return;
                }
                LOGV2_ERROR(21037,
                            "Error scheduling new oplog query: {scheduleStatus}. Returning current "
                            "oplog query error: {responseStatus}",
                            "scheduleStatus"_attr = redact(scheduleStatus),
                            "responseStatus"_attr = redact(responseStatus));
            }
        }
        _finishCallback(responseStatus);
        return;
    }

    // Reset fetcher restart counter on successful response.
    {
        stdx::lock_guard<Latch> lock(_mutex);
        invariant(_isActive_inlock());
        _oplogFetcherRestartDecision->fetchSuccessful(this);
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
        auto lastDocRes = OpTime::parseFromOplogEntry(documents.back());
        if (!lastDocRes.isOK()) {
            _finishCallback(lastDocRes.getStatus());
            return;
        }
        auto lastDoc = lastDocRes.getValue();
        LOGV2_DEBUG(21034,
                    3,
                    "{getComponentName} setting last fetched optime ahead after batch: {lastDoc}",
                    "getComponentName"_attr = _getComponentName(),
                    "lastDoc"_attr = lastDoc);

        stdx::lock_guard<Latch> lock(_mutex);
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
    decltype(_oplogFetcherRestartDecision) oplogFetcherRestartDecision;
    stdx::lock_guard<Latch> lock(_mutex);
    _transitionToComplete_inlock();

    // Release any resources that might be held by the '_onShutdownCallbackFn' function object.
    // The function object will be destroyed outside the lock since the temporary variable
    // 'onShutdownCallbackFn' is declared before 'lock'.
    invariant(_onShutdownCallbackFn);
    std::swap(_onShutdownCallbackFn, onShutdownCallbackFn);

    // Release any resources held by the OplogFetcherRestartDecision
    invariant(_oplogFetcherRestartDecision);
    std::swap(_oplogFetcherRestartDecision, oplogFetcherRestartDecision);
}

std::unique_ptr<Fetcher> AbstractOplogFetcher::_makeFetcher(const BSONObj& findCommandObj,
                                                            const BSONObj& metadataObj,
                                                            Milliseconds findMaxTime) {
    return std::make_unique<Fetcher>(
        _getExecutor(),
        _source,
        _nss.db().toString(),
        findCommandObj,
        [this](const StatusWith<Fetcher::QueryResponse>& resp,
               Fetcher::NextAction*,
               BSONObjBuilder* builder) { return _callback(resp, builder); },
        metadataObj,
        findMaxTime + _getNetworkTimeoutBuffer(),
        _getGetMoreMaxTime() + _getNetworkTimeoutBuffer());
}

bool AbstractOplogFetcher::OplogFetcherRestartDecisionDefault::shouldContinue(
    AbstractOplogFetcher* fetcher, Status status) {
    if (_fetcherRestarts == _maxFetcherRestarts) {
        LOGV2(21035,
              "Error returned from oplog query (no more query restarts left): {status}",
              "status"_attr = redact(status));
        return false;
    }
    LOGV2(
        21036,
        "Restarting oplog query due to error: {status}. Last fetched optime: "
        "{fetcher_getLastOpTimeFetched}. Restarts remaining: {maxFetcherRestarts_fetcherRestarts}",
        "status"_attr = redact(status),
        "fetcher_getLastOpTimeFetched"_attr = fetcher->_getLastOpTimeFetched(),
        "maxFetcherRestarts_fetcherRestarts"_attr = (_maxFetcherRestarts - _fetcherRestarts));
    _fetcherRestarts++;
    return true;
}

void AbstractOplogFetcher::OplogFetcherRestartDecisionDefault::fetchSuccessful(
    AbstractOplogFetcher* fetcher) {
    _fetcherRestarts = 0;
};

AbstractOplogFetcher::OplogFetcherRestartDecision::~OplogFetcherRestartDecision(){};

}  // namespace repl
}  // namespace mongo
