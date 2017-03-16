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
#include "mongo/rpc/metadata/oplog_query_metadata.h"
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

MONGO_FP_DECLARE(stopReplProducer);

namespace {

Seconds kOplogInitialFindMaxTime{60};
Seconds kOplogQueryNetworkTimeout{65};  // 5 seconds past the find command's 1 minute maxTimeMs

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

/**
 * Calculates await data timeout based on the current replica set configuration.
 */
Milliseconds calculateAwaitDataTimeout(const ReplSetConfig& config) {
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
BSONObj makeFindCommandObject(const NamespaceString& nss,
                              long long currentTerm,
                              OpTime lastOpTimeFetched) {
    BSONObjBuilder cmdBob;
    cmdBob.append("find", nss.coll());
    cmdBob.append("filter", BSON("ts" << BSON("$gte" << lastOpTimeFetched.getTimestamp())));
    cmdBob.append("tailable", true);
    cmdBob.append("oplogReplay", true);
    cmdBob.append("awaitData", true);
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(kOplogInitialFindMaxTime));
    if (currentTerm != OpTime::kUninitializedTerm) {
        cmdBob.append("term", currentTerm);
    }
    return cmdBob.obj();
}

/**
 * Returns getMore command object suitable for tailing remote oplog.
 */
BSONObj makeGetMoreCommandObject(const NamespaceString& nss,
                                 CursorId cursorId,
                                 OpTimeWithTerm lastCommittedWithCurrentTerm,
                                 Milliseconds fetcherMaxTimeMS) {
    BSONObjBuilder cmdBob;
    cmdBob.append("getMore", cursorId);
    cmdBob.append("collection", nss.coll());
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(fetcherMaxTimeMS));
    if (lastCommittedWithCurrentTerm.value != OpTime::kUninitializedTerm) {
        cmdBob.append("term", lastCommittedWithCurrentTerm.value);
        lastCommittedWithCurrentTerm.opTime.append(&cmdBob, "lastKnownCommittedOpTime");
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
               << rpc::kOplogQueryMetadataFieldName
               << 1
               << rpc::ServerSelectionMetadata::fieldName()
               << BSON(rpc::ServerSelectionMetadata::kSecondaryOkFieldName << true))
        : rpc::ServerSelectionMetadata(true, boost::none).toBSON();
}

/**
 * Checks the first batch of results from query.
 * 'documents' are the first batch of results returned from tailing the remote oplog.
 * 'lastFetched' optime and hash should be consistent with the predicate in the query.
 * 'remoteLastOpApplied' is the last OpTime applied on the sync source. This is optional for
 * compatibility with 3.4 servers that do not send OplogQueryMetadata.
 * 'requiredRBID' is a RollbackID received when we chose the sync source that we use here to
 * guarantee we have not rolled back since we confirmed the sync source had our minValid.
 * 'remoteRBID' is a RollbackId for the sync source returned in this oplog query. This is optional
 * for compatibility with 3.4 servers that do not send OplogQueryMetadata.
 * 'requireFresherSyncSource' is a boolean indicating whether we should require the sync source's
 * oplog to be ahead of ours. If false, the sync source's oplog is allowed to be at the same point
 * as ours, but still cannot be behind ours.
 *
 * TODO (SERVER-27668): Make remoteLastOpApplied and remoteRBID non-optional in mongodb 3.8.
 *
 * Returns OplogStartMissing if we cannot find the optime of the last fetched operation in
 * the remote oplog.
 */
Status checkRemoteOplogStart(const Fetcher::Documents& documents,
                             OpTimeWithHash lastFetched,
                             boost::optional<OpTime> remoteLastOpApplied,
                             int requiredRBID,
                             boost::optional<int> remoteRBID,
                             bool requireFresherSyncSource) {
    // Once we establish our cursor, we need to ensure that our upstream node hasn't rolled back
    // since that could cause it to not have our required minValid point. The cursor will be
    // killed if the upstream node rolls back so we don't need to keep checking once the cursor
    // is established.
    if (remoteRBID && (*remoteRBID != requiredRBID)) {
        return Status(ErrorCodes::InvalidSyncSource,
                      "Upstream node rolled back after choosing it as a sync source. Choosing "
                      "new sync source.");
    }

    // The SyncSourceResolver never checks that the sync source candidate is actually ahead of
    // us. Rather than have it check there with an extra network roundtrip, we check here.
    if (requireFresherSyncSource && remoteLastOpApplied &&
        (*remoteLastOpApplied <= lastFetched.opTime)) {
        return Status(ErrorCodes::InvalidSyncSource,
                      str::stream() << "Sync source's last applied OpTime "
                                    << remoteLastOpApplied->toString()
                                    << " is not greater than our last fetched OpTime "
                                    << lastFetched.opTime.toString()
                                    << ". Choosing new sync source.");
    } else if (remoteLastOpApplied && (*remoteLastOpApplied < lastFetched.opTime)) {
        // In initial sync, the lastFetched OpTime will almost always equal the remoteLastOpApplied
        // since we fetch the sync source's last applied OpTime to determine where to start our
        // OplogFetcher. This is fine since no other node can sync off of an initial syncing node
        // and thus cannot form a sync source cycle. To account for this, we must relax the
        // constraint on our sync source being fresher.
        return Status(ErrorCodes::InvalidSyncSource,
                      str::stream() << "Sync source's last applied OpTime "
                                    << remoteLastOpApplied->toString()
                                    << " is older than our last fetched OpTime "
                                    << lastFetched.opTime.toString()
                                    << ". Choosing new sync source.");
    }

    // At this point we know that our sync source has our minValid and is ahead of us, so if our
    // history diverges from our sync source's we should prefer its history and roll back ours.

    // Since we checked for rollback and our sync source is ahead of us, an empty batch means that
    // we have a higher timestamp on our last fetched OpTime than our sync source's last applied
    // OpTime, but a lower term. When this occurs, we must roll back our inconsistent oplog entry.
    if (documents.empty()) {
        return Status(ErrorCodes::OplogStartMissing, "Received an empty batch from sync source.");
    }

    const auto& o = documents.front();
    auto opTimeResult = OpTime::parseFromOplogEntry(o);
    if (!opTimeResult.isOK()) {
        return Status(ErrorCodes::InvalidBSON,
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

/**
 * Parses a QueryResponse for the OplogQueryMetadata. If there is an error it returns it. If
 * no OplogQueryMetadata is provided then it returns boost::none.
 *
 * OplogQueryMetadata is made optional for backwards compatibility.
 * TODO (SERVER-27668): Make this non-optional in mongodb 3.8. When this stops being optional
 * we can remove the duplicated fields in both metadata types and begin to always use
 * OplogQueryMetadata's data.
 */
StatusWith<boost::optional<rpc::OplogQueryMetadata>> parseOplogQueryMetadata(
    Fetcher::QueryResponse queryResponse) {
    boost::optional<rpc::OplogQueryMetadata> oqMetadata = boost::none;
    bool receivedOplogQueryMetadata =
        queryResponse.otherFields.metadata.hasElement(rpc::kOplogQueryMetadataFieldName);
    if (receivedOplogQueryMetadata) {
        const auto& metadataObj = queryResponse.otherFields.metadata;
        auto metadataResult = rpc::OplogQueryMetadata::readFromMetadata(metadataObj);
        if (!metadataResult.isOK()) {
            return metadataResult.getStatus();
        }
        oqMetadata = boost::make_optional<rpc::OplogQueryMetadata>(metadataResult.getValue());
    }
    return oqMetadata;
}
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
                           ReplSetConfig config,
                           std::size_t maxFetcherRestarts,
                           int requiredRBID,
                           bool requireFresherSyncSource,
                           DataReplicatorExternalState* dataReplicatorExternalState,
                           EnqueueDocumentsFn enqueueDocumentsFn,
                           OnShutdownCallbackFn onShutdownCallbackFn)
    : AbstractAsyncComponent(executor, "oplog fetcher"),
      _source(source),
      _nss(nss),
      _metadataObject(uassertStatusOK(makeMetadataObject(config.getProtocolVersion() == 1LL))),
      _maxFetcherRestarts(maxFetcherRestarts),
      _requiredRBID(requiredRBID),
      _requireFresherSyncSource(requireFresherSyncSource),
      _dataReplicatorExternalState(dataReplicatorExternalState),
      _enqueueDocumentsFn(enqueueDocumentsFn),
      _awaitDataTimeout(calculateAwaitDataTimeout(config)),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(lastFetched) {
    uassert(ErrorCodes::BadValue, "null last optime fetched", !_lastFetched.opTime.isNull());
    uassert(ErrorCodes::InvalidReplicaSetConfig,
            "uninitialized replica set configuration",
            config.isInitialized());
    uassert(ErrorCodes::BadValue, "null enqueueDocuments function", enqueueDocumentsFn);
    uassert(ErrorCodes::BadValue, "null onShutdownCallback function", onShutdownCallbackFn);

    auto currentTerm = dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime().value;
    _fetcher = _makeFetcher(currentTerm, _lastFetched.opTime);
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

Status OplogFetcher::_doStartup_inlock() noexcept {
    return _scheduleFetcher_inlock();
}

void OplogFetcher::_doShutdown_inlock() noexcept {
    _fetcher->shutdown();
}

stdx::mutex* OplogFetcher::_getMutex() noexcept {
    return &_mutex;
}

Status OplogFetcher::_scheduleFetcher_inlock() {
    readersCreatedStats.increment();
    return _fetcher->schedule();
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

Milliseconds OplogFetcher::getAwaitDataTimeout_forTest() const {
    return _awaitDataTimeout;
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
            // We have to call into replication coordinator outside oplog fetcher's mutex.
            // It is OK if the current term becomes stale after this line since requests
            // to remote nodes are asynchronous anyway.
            auto currentTerm =
                _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime().value;
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_isShuttingDown_inlock()) {
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
                // Create and start fetcher with current term and new starting optime.
                _fetcher = _makeFetcher(currentTerm, _lastFetched.opTime);
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
        _finishCallback(Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down"));
        return;
    }

    // Stop fetching and return on fail point.
    // This fail point makes the oplog fetcher ignore the downloaded batch of operations and not
    // error out.
    if (MONGO_FAIL_POINT(stopReplProducer)) {
        _finishCallback(Status::OK());
        return;
    }

    const auto& queryResponse = result.getValue();
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

    auto oqMetadataResult = parseOplogQueryMetadata(queryResponse);
    if (!oqMetadataResult.isOK()) {
        error() << "invalid oplog query metadata from sync source " << _fetcher->getSource() << ": "
                << oqMetadataResult.getStatus() << ": " << queryResponse.otherFields.metadata;
        _finishCallback(oqMetadataResult.getStatus());
        return;
    }
    auto oqMetadata = oqMetadataResult.getValue();

    // Check start of remote oplog and, if necessary, stop fetcher to execute rollback.
    if (queryResponse.first) {
        auto remoteRBID = oqMetadata ? boost::make_optional(oqMetadata->getRBID()) : boost::none;
        auto remoteLastApplied =
            oqMetadata ? boost::make_optional(oqMetadata->getLastOpApplied()) : boost::none;
        auto status = checkRemoteOplogStart(documents,
                                            opTimeWithHash,
                                            remoteLastApplied,
                                            _requiredRBID,
                                            remoteRBID,
                                            _requireFresherSyncSource);
        if (!status.isOK()) {
            // Stop oplog fetcher and execute rollback if necessary.
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

    // Process replset metadata.  It is important that this happen after we've validated the
    // first batch, so we don't progress our knowledge of the commit point from a
    // response that triggers a rollback.
    rpc::ReplSetMetadata replSetMetadata;
    bool receivedReplMetadata =
        queryResponse.otherFields.metadata.hasElement(rpc::kReplSetMetadataFieldName);
    if (receivedReplMetadata) {
        const auto& metadataObj = queryResponse.otherFields.metadata;
        auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(metadataObj);
        if (!metadataResult.isOK()) {
            error() << "invalid replication metadata from sync source " << _fetcher->getSource()
                    << ": " << metadataResult.getStatus() << ": " << metadataObj;
            _finishCallback(metadataResult.getStatus());
            return;
        }
        replSetMetadata = metadataResult.getValue();

        // We will only ever have OplogQueryMetadata if we have ReplSetMetadata, so it is safe
        // to call processMetadata() in this if block.
        _dataReplicatorExternalState->processMetadata(replSetMetadata, oqMetadata);
    }

    // Increment stats. We read all of the docs in the query.
    opsReadStats.increment(info.networkDocumentCount);
    networkByteStats.increment(info.networkDocumentBytes);

    // Record time for each batch.
    getmoreReplStats.recordMillis(durationCount<Milliseconds>(queryResponse.elapsedMillis));

    // TODO: back pressure handling will be added in SERVER-23499.
    auto status = _enqueueDocumentsFn(firstDocToApply, documents.cend(), info);
    if (!status.isOK()) {
        _finishCallback(status);
        return;
    }

    // Update last fetched info.
    if (firstDocToApply != documents.cend()) {
        opTimeWithHash = info.lastDocument;
        LOG(3) << "batch resetting last fetched optime: " << opTimeWithHash.opTime
               << "; hash: " << opTimeWithHash.value;

        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _lastFetched = opTimeWithHash;
    }

    if (_dataReplicatorExternalState->shouldStopFetching(
            _fetcher->getSource(), replSetMetadata, oqMetadata)) {
        str::stream errMsg;
        errMsg << "sync source " << _fetcher->getSource().toString();
        errMsg << " (config version: " << replSetMetadata.getConfigVersion();
        // If OplogQueryMetadata was provided, its values were used to determine if we should
        // stop fetching from this sync source.
        if (oqMetadata) {
            errMsg << "; last applied optime: " << oqMetadata->getLastOpApplied().toString();
            errMsg << "; sync source index: " << oqMetadata->getSyncSourceIndex();
            errMsg << "; primary index: " << oqMetadata->getPrimaryIndex();
        } else {
            errMsg << "; last visible optime: " << replSetMetadata.getLastOpVisible().toString();
            errMsg << "; sync source index: " << replSetMetadata.getSyncSourceIndex();
            errMsg << "; primary index: " << replSetMetadata.getPrimaryIndex();
        }
        errMsg << ") is no longer valid";
        _finishCallback(Status(ErrorCodes::InvalidSyncSource, errMsg), opTimeWithHash);
        return;
    }

    // No more data. Stop processing and return Status::OK along with last
    // fetch info.
    if (!getMoreBob) {
        _finishCallback(Status::OK(), opTimeWithHash);
        return;
    }

    auto lastCommittedWithCurrentTerm =
        _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    getMoreBob->appendElements(makeGetMoreCommandObject(queryResponse.nss,
                                                        queryResponse.cursorId,
                                                        lastCommittedWithCurrentTerm,
                                                        _awaitDataTimeout));
}

void OplogFetcher::_finishCallback(Status status) {
    _finishCallback(status, getLastOpTimeWithHashFetched());
}

void OplogFetcher::_finishCallback(Status status, OpTimeWithHash opTimeWithHash) {
    invariant(isActive());

    _onShutdownCallbackFn(status, opTimeWithHash);

    decltype(_onShutdownCallbackFn) onShutdownCallbackFn;
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _transitionToComplete_inlock();

    // Release any resources that might be held by the '_onShutdownCallbackFn' function object.
    // The function object will be destroyed outside the lock since the temporary variable
    // 'onShutdownCallbackFn' is declared before 'lock'.
    invariant(_onShutdownCallbackFn);
    std::swap(_onShutdownCallbackFn, onShutdownCallbackFn);
}

std::unique_ptr<Fetcher> OplogFetcher::_makeFetcher(long long currentTerm,
                                                    OpTime lastFetchedOpTime) {
    return stdx::make_unique<Fetcher>(
        _getExecutor(),
        _source,
        _nss.db().toString(),
        makeFindCommandObject(_nss, currentTerm, lastFetchedOpTime),
        stdx::bind(&OplogFetcher::_callback, this, stdx::placeholders::_1, stdx::placeholders::_3),
        _metadataObject,
        kOplogQueryNetworkTimeout);
}

}  // namespace repl
}  // namespace mongo
