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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_fetcher.h"

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

Seconds OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout(2);

namespace {

MONGO_FP_DECLARE(stopOplogFetcher);

/**
 * Calculates await data timeout based on the current replica set configuration.
 */
Milliseconds calculateAwaitDataTimeout(const ReplicaSetConfig& config) {
    // Under protocol version 1, make the awaitData timeout (maxTimeMS) dependent on the election
    // timeout. This enables the sync source to communicate liveness of the primary to secondaries.
    // Under protocol version 0, use a default timeout of 2 seconds for awaitData.
    if (config.getProtocolVersion() == 1LL) {
        return config.getElectionTimeoutPeriod() / 2;
    }
    return OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout;
}

/**
 * Returns find command object suitable for tailing remote oplog.
 */
BSONObj makeFindCommandObject(DataReplicatorExternalState* dataReplicatorExternalState,
                              const NamespaceString& nss,
                              OpTime lastOpTimeFetched) {
    invariant(dataReplicatorExternalState);
    BSONObjBuilder cmdBob;
    cmdBob.append("find", nss.coll());
    cmdBob.append("filter", BSON("ts" << BSON("$gte" << lastOpTimeFetched.getTimestamp())));
    cmdBob.append("tailable", true);
    cmdBob.append("oplogReplay", true);
    cmdBob.append("awaitData", true);
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(Minutes(1)));  // 1 min initial find.
    auto opTimeWithTerm = dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    if (opTimeWithTerm.value != OpTime::kUninitializedTerm) {
        cmdBob.append("term", opTimeWithTerm.value);
    }
    return cmdBob.obj();
}

/**
 * Returns getMore command object suitable for tailing remote oplog.
 */
BSONObj makeGetMoreCommandObject(DataReplicatorExternalState* dataReplicatorExternalState,
                                 const NamespaceString& nss,
                                 CursorId cursorId,
                                 Milliseconds fetcherMaxTimeMS) {
    BSONObjBuilder cmdBob;
    cmdBob.append("getMore", cursorId);
    cmdBob.append("collection", nss.coll());
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(fetcherMaxTimeMS));
    auto opTimeWithTerm = dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    if (opTimeWithTerm.value != OpTime::kUninitializedTerm) {
        cmdBob.append("term", opTimeWithTerm.value);
        opTimeWithTerm.opTime.append(&cmdBob, "lastKnownCommittedOpTime");
    }
    return cmdBob.obj();
}

/**
 * Returns command metadata object suitable for tailing remote oplog.
 */
StatusWith<BSONObj> makeMetadataObject(bool isV1ElectionProtocol) {
    return isV1ElectionProtocol
        ? BSON(rpc::kReplSetMetadataFieldName
               << 1
               << rpc::ServerSelectionMetadata::fieldName()
               << BSON(rpc::ServerSelectionMetadata::kSecondaryOkFieldName << true))
        : rpc::ServerSelectionMetadata(true, boost::none).toBSON();
}

/**
 * Checks the first batch of results from query.
 * 'documents' are the first batch of results returned from tailing the remote oplog.
 * 'lastFetched' optime and hash should be consistent with the predicate in the query.
 * Returns RemoteOplogStale if the oplog query has no results.
 * Returns OplogStartMissing if we cannot find the optime of the last fetched operation in
 * the remote oplog.
 */
Status checkRemoteOplogStart(const Fetcher::Documents& documents, OpTimeWithHash lastFetched) {
    if (documents.empty()) {
        // The GTE query from upstream returns nothing, so we're ahead of the upstream.
        return Status(ErrorCodes::RemoteOplogStale,
                      str::stream() << "We are ahead of the sync source. Our last op time fetched: "
                                    << lastFetched.opTime.toString());
    }
    const auto& o = documents.front();
    auto opTimeResult = OpTime::parseFromOplogEntry(o);
    if (!opTimeResult.isOK()) {
        return Status(ErrorCodes::OplogStartMissing,
                      str::stream() << "our last op time fetched: " << lastFetched.opTime.toString()
                                    << " (hash: "
                                    << lastFetched.value
                                    << ")"
                                    << ". failed to parse optime from first oplog on source: "
                                    << o.toString()
                                    << ": "
                                    << opTimeResult.getStatus().toString());
    }
    auto opTime = opTimeResult.getValue();
    long long hash = o["h"].numberLong();
    if (opTime != lastFetched.opTime || hash != lastFetched.value) {
        return Status(ErrorCodes::OplogStartMissing,
                      str::stream() << "our last op time fetched: " << lastFetched.opTime.toString()
                                    << ". source's GTE: "
                                    << opTime.toString()
                                    << " hashes: ("
                                    << lastFetched.value
                                    << "/"
                                    << hash
                                    << ")");
    }
    return Status::OK();
}

Counter64 readersCreatedStats;
ServerStatusMetricField<Counter64> displayReadersCreated("repl.network.readersCreated",
                                                         &readersCreatedStats);
// The number and time spent reading batches off the network
TimerStats getmoreReplStats;
ServerStatusMetricField<TimerStats> displayBatchesRecieved("repl.network.getmores",
                                                           &getmoreReplStats);
// The oplog entries read via the oplog reader
Counter64 opsReadStats;
ServerStatusMetricField<Counter64> displayOpsRead("repl.network.ops", &opsReadStats);
// The bytes read via the oplog reader
Counter64 networkByteStats;
ServerStatusMetricField<Counter64> displayBytesRead("repl.network.bytes", &networkByteStats);

}  // namespace

StatusWith<OplogFetcher::DocumentsInfo> OplogFetcher::validateDocuments(
    const Fetcher::Documents& documents, bool first, Timestamp lastTS) {
    if (first && documents.empty()) {
        return Status(ErrorCodes::OplogStartMissing,
                      str::stream() << "The first batch of oplog entries is empty, but expected at "
                                       "least 1 document matching ts: "
                                    << lastTS.toString());
    }

    DocumentsInfo info;
    // The count of the bytes of the documents read off the network.
    info.networkDocumentBytes = 0;
    info.networkDocumentCount = 0;
    for (auto&& doc : documents) {
        info.networkDocumentBytes += doc.objsize();
        ++info.networkDocumentCount;

        // If this is the first response (to the $gte query) then we already applied the first doc.
        if (first && info.networkDocumentCount == 1U) {
            continue;
        }

        // Check to see if the oplog entry goes back in time for this document.
        const auto docOpTime = OpTime::parseFromOplogEntry(doc);
        // entries must have a "ts" field.
        if (!docOpTime.isOK()) {
            return docOpTime.getStatus();
        }

        info.lastDocument = {doc["h"].numberLong(), docOpTime.getValue()};

        const auto docTS = info.lastDocument.opTime.getTimestamp();
        if (lastTS >= docTS) {
            return Status(ErrorCodes::OplogOutOfOrder,
                          str::stream() << "Out of order entries in oplog. lastTS: "
                                        << lastTS.toString()
                                        << " outOfOrderTS:"
                                        << docTS.toString()
                                        << " in batch with "
                                        << info.networkDocumentCount
                                        << "docs; first-batch:"
                                        << first
                                        << ", doc:"
                                        << doc);
        }
        lastTS = docTS;
    }

    // These numbers are for the documents we will apply.
    info.toApplyDocumentCount = documents.size();
    info.toApplyDocumentBytes = info.networkDocumentBytes;
    if (first) {
        // The count is one less since the first document found was already applied ($gte $ts query)
        // and we will not apply it again.
        --info.toApplyDocumentCount;
        auto alreadyAppliedDocument = documents.cbegin();
        info.toApplyDocumentBytes -= alreadyAppliedDocument->objsize();
    }
    return info;
}

OplogFetcher::OplogFetcher(executor::TaskExecutor* executor,
                           OpTimeWithHash lastFetched,
                           HostAndPort source,
                           NamespaceString nss,
                           ReplicaSetConfig config,
                           std::size_t maxFetcherRestarts,
                           DataReplicatorExternalState* dataReplicatorExternalState,
                           EnqueueDocumentsFn enqueueDocumentsFn,
                           OnShutdownCallbackFn onShutdownCallbackFn)
    : _executor(executor),
      _source(source),
      _nss(nss),
      _metadataObject(uassertStatusOK(makeMetadataObject(config.getProtocolVersion() == 1LL))),
      _remoteCommandTimeout(config.getElectionTimeoutPeriod()),
      _maxFetcherRestarts(maxFetcherRestarts),
      _dataReplicatorExternalState(dataReplicatorExternalState),
      _enqueueDocumentsFn(enqueueDocumentsFn),
      _awaitDataTimeout(calculateAwaitDataTimeout(config)),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(lastFetched),
      _fetcher(_makeFetcher(_lastFetched.opTime)) {
    uassert(ErrorCodes::BadValue, "null last optime fetched", !_lastFetched.opTime.isNull());
    uassert(ErrorCodes::InvalidReplicaSetConfig,
            "uninitialized replica set configuration",
            config.isInitialized());
    uassert(ErrorCodes::BadValue, "null enqueueDocuments function", enqueueDocumentsFn);
    uassert(ErrorCodes::BadValue, "null onShutdownCallback function", onShutdownCallbackFn);
}

OplogFetcher::~OplogFetcher() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

std::string OplogFetcher::toString() const {
    return str::stream() << "OplogReader -"
                         << " last optime fetched: " << _lastFetched.opTime.toString()
                         << " last hash fetched: " << _lastFetched.value
                         << " fetcher: " << _fetcher->getDiagnosticString();
}

bool OplogFetcher::isActive() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _active;
}

Status OplogFetcher::startup() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "oplog fetcher already active");
    }
    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress, "oplog fetcher shutting down");
    }

    auto status = _scheduleFetcher_inlock();
    if (status.isOK()) {
        _active = true;
    }
    return status;
}

Status OplogFetcher::_scheduleFetcher_inlock() {
    readersCreatedStats.increment();
    return _fetcher->schedule();
}

void OplogFetcher::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _inShutdown = true;
    _fetcher->shutdown();
}

void OplogFetcher::join() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condition.wait(lock, [this]() { return !_active; });
}

OpTimeWithHash OplogFetcher::getLastOpTimeWithHashFetched() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _lastFetched;
}

BSONObj OplogFetcher::getCommandObject_forTest() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _fetcher->getCommandObject();
}

BSONObj OplogFetcher::getMetadataObject_forTest() const {
    return _metadataObject;
}

Milliseconds OplogFetcher::getRemoteCommandTimeout_forTest() const {
    return _remoteCommandTimeout;
}

Milliseconds OplogFetcher::getAwaitDataTimeout_forTest() const {
    return _awaitDataTimeout;
}

bool OplogFetcher::inShutdown_forTest() const {
    return _isInShutdown();
}

void OplogFetcher::_callback(const Fetcher::QueryResponseStatus& result,
                             BSONObjBuilder* getMoreBob) {
    const auto& responseStatus = result.getStatus();
    if (ErrorCodes::CallbackCanceled == responseStatus) {
        LOG(1) << "oplog query cancelled";
        _finishCallback(responseStatus);
        return;
    }

    // If target cut connections between connecting and querying (for
    // example, because it stepped down) we might not have a cursor.
    if (!responseStatus.isOK()) {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_inShutdown) {
                log() << "Error returned from oplog query while canceling query: "
                      << redact(responseStatus);
            } else if (_fetcherRestarts == _maxFetcherRestarts) {
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
                // Create and start fetcher with new starting optime.
                _fetcher = _makeFetcher(_lastFetched.opTime);
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
        invariant(_active);
        _fetcherRestarts = 0;
    }

    if (_isInShutdown()) {
        _finishCallback(Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down"));
        return;
    }

    // Stop fetching and return immediately on fail point.
    // This fail point is intended to make the oplog fetcher ignore the downloaded batch of
    // operations and not error out.
    if (MONGO_FAIL_POINT(stopOplogFetcher)) {
        _finishCallback(Status::OK());
        return;
    }

    const auto& queryResponse = result.getValue();
    rpc::ReplSetMetadata metadata;

    // Forward metadata (containing liveness information) to data replicator external state.
    bool receivedMetadata =
        queryResponse.otherFields.metadata.hasElement(rpc::kReplSetMetadataFieldName);
    if (receivedMetadata) {
        const auto& metadataObj = queryResponse.otherFields.metadata;
        auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(metadataObj);
        if (!metadataResult.isOK()) {
            error() << "invalid replication metadata from sync source " << _fetcher->getSource()
                    << ": " << metadataResult.getStatus() << ": " << metadataObj;
            _finishCallback(metadataResult.getStatus());
            return;
        }
        metadata = metadataResult.getValue();
        _dataReplicatorExternalState->processMetadata(metadata);
    }

    const auto& documents = queryResponse.documents;
    auto firstDocToApply = documents.cbegin();

    if (!documents.empty()) {
        LOG(2) << "oplog fetcher read " << documents.size()
               << " operations from remote oplog starting at " << documents.front()["ts"]
               << " and ending at " << documents.back()["ts"];
    } else {
        LOG(2) << "oplog fetcher read 0 operations from remote oplog";
    }

    auto opTimeWithHash = getLastOpTimeWithHashFetched();

    // Check start of remote oplog and, if necessary, stop fetcher to execute rollback.
    if (queryResponse.first) {
        auto status = checkRemoteOplogStart(documents, opTimeWithHash);
        if (!status.isOK()) {
            // Stop oplog fetcher and execute rollback.
            _finishCallback(status, opTimeWithHash);
            return;
        }

        LOG(1) << "oplog fetcher successfully fetched from " << _source;

        // If this is the first batch and no rollback is needed, skip the first document.
        firstDocToApply++;
    }

    auto validateResult = OplogFetcher::validateDocuments(
        documents, queryResponse.first, opTimeWithHash.opTime.getTimestamp());
    if (!validateResult.isOK()) {
        _finishCallback(validateResult.getStatus(), opTimeWithHash);
        return;
    }
    auto info = validateResult.getValue();

    // Increment stats. We read all of the docs in the query.
    opsReadStats.increment(info.networkDocumentCount);
    networkByteStats.increment(info.networkDocumentBytes);

    // Record time for each batch.
    getmoreReplStats.recordMillis(durationCount<Milliseconds>(queryResponse.elapsedMillis));

    // TODO: back pressure handling will be added in SERVER-23499.
    _enqueueDocumentsFn(firstDocToApply, documents.cend(), info);

    // Update last fetched info.
    if (firstDocToApply != documents.cend()) {
        opTimeWithHash = info.lastDocument;
        LOG(3) << "batch resetting last fetched optime: " << opTimeWithHash.opTime
               << "; hash: " << opTimeWithHash.value;

        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _lastFetched = opTimeWithHash;
    }

    if (_dataReplicatorExternalState->shouldStopFetching(_fetcher->getSource(), metadata)) {
        _finishCallback(Status(ErrorCodes::InvalidSyncSource,
                               str::stream() << "sync source " << _fetcher->getSource().toString()
                                             << " (last optime: "
                                             << metadata.getLastOpVisible().toString()
                                             << "; sync source index: "
                                             << metadata.getSyncSourceIndex()
                                             << "; primary index: "
                                             << metadata.getPrimaryIndex()
                                             << ") is no longer valid"),
                        opTimeWithHash);
        return;
    }

    // No more data. Stop processing and return Status::OK along with last
    // fetch info.
    if (!getMoreBob) {
        _finishCallback(Status::OK(), opTimeWithHash);
        return;
    }

    getMoreBob->appendElements(makeGetMoreCommandObject(_dataReplicatorExternalState,
                                                        queryResponse.nss,
                                                        queryResponse.cursorId,
                                                        _awaitDataTimeout));
}

void OplogFetcher::_finishCallback(Status status) {
    _finishCallback(status, getLastOpTimeWithHashFetched());
}

void OplogFetcher::_finishCallback(Status status, OpTimeWithHash opTimeWithHash) {
    invariant(isActive());

    _onShutdownCallbackFn(status, opTimeWithHash);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _active = false;
    _condition.notify_all();
}

std::unique_ptr<Fetcher> OplogFetcher::_makeFetcher(OpTime lastFetchedOpTime) {
    return stdx::make_unique<Fetcher>(
        _executor,
        _source,
        _nss.db().toString(),
        makeFindCommandObject(_dataReplicatorExternalState, _nss, lastFetchedOpTime),
        stdx::bind(&OplogFetcher::_callback, this, stdx::placeholders::_1, stdx::placeholders::_3),
        _metadataObject,
        _remoteCommandTimeout);
}

bool OplogFetcher::_isInShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _inShutdown;
}

}  // namespace repl
}  // namespace mongo
