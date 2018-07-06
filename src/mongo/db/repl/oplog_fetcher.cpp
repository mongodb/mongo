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
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

Seconds OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout(2);

MONGO_FP_DECLARE(stopReplProducer);

namespace {

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

const Milliseconds maximumAwaitDataTimeoutMS(30 * 1000);

/**
 * Calculates await data timeout based on the current replica set configuration.
 */
Milliseconds calculateAwaitDataTimeout(const ReplSetConfig& config) {
    // Under protocol version 1, make the awaitData timeout (maxTimeMS) dependent on the election
    // timeout. This enables the sync source to communicate liveness of the primary to secondaries.
    // We never wait longer than 30 seconds.
    // Under protocol version 0, use a default timeout of 2 seconds for awaitData.
    if (config.getProtocolVersion() == 1LL) {
        return std::min((config.getElectionTimeoutPeriod() / 2), maximumAwaitDataTimeoutMS);
    }
    return OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout;
}

/**
 * Returns getMore command object suitable for tailing remote oplog.
 */
BSONObj makeGetMoreCommandObject(const NamespaceString& nss,
                                 CursorId cursorId,
                                 OpTimeWithTerm lastCommittedWithCurrentTerm,
                                 Milliseconds fetcherMaxTimeMS,
                                 int batchSize) {
    BSONObjBuilder cmdBob;
    cmdBob.append("getMore", cursorId);
    cmdBob.append("collection", nss.coll());
    cmdBob.append("batchSize", batchSize);
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
BSONObj makeMetadataObject(bool isV1ElectionProtocol) {
    if (!isV1ElectionProtocol)
        return ReadPreferenceSetting::secondaryPreferredMetadata();

    BSONObjBuilder metaBuilder;
    metaBuilder << rpc::kReplSetMetadataFieldName << 1;
    metaBuilder << rpc::kOplogQueryMetadataFieldName << 1;
    metaBuilder.appendElements(ReadPreferenceSetting::secondaryPreferredMetadata());
    return metaBuilder.obj();
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

    // Sometimes our remoteLastOpApplied may be stale; if we received a document with an
    // opTime later than remoteLastApplied, we can assume the remote is at least up to that
    // opTime.
    if (remoteLastOpApplied && !documents.empty()) {
        const auto docOpTime = OpTime::parseFromOplogEntry(documents.back());
        if (docOpTime.isOK()) {
            remoteLastOpApplied = std::max(*remoteLastOpApplied, docOpTime.getValue());
        }
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
        std::string message = str::stream()
            << "Our last op time fetched: " << lastFetched.opTime.toString()
            << ". source's GTE: " << opTime.toString() << " hashes: (" << lastFetched.value << "/"
            << hash << ")";

        // In PV1, if the hashes do not match, the optimes should not either since optimes uniquely
        // identify oplog entries. In that case we fail before we potentially corrupt data. This
        // should never happen.
        if (opTime.getTerm() != OpTime::kUninitializedTerm && hash != lastFetched.value &&
            opTime == lastFetched.opTime) {
            severe() << "Hashes do not match but OpTimes do. " << message
                     << ". Source's GTE doc: " << redact(o);
            fassertFailedNoTrace(40634);
        }

        return Status(ErrorCodes::OplogStartMissing, message);
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
        oqMetadata = boost::make_optional(metadataResult.getValue());
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

        auto docOpTimeWithHash = AbstractOplogFetcher::parseOpTimeWithHash(doc);
        if (!docOpTimeWithHash.isOK()) {
            return docOpTimeWithHash.getStatus();
        }
        info.lastDocument = docOpTimeWithHash.getValue();

        // Check to see if the oplog entry goes back in time for this document.
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
                           OnShutdownCallbackFn onShutdownCallbackFn,
                           const int batchSize)
    : AbstractOplogFetcher(executor,
                           lastFetched,
                           source,
                           nss,
                           maxFetcherRestarts,
                           onShutdownCallbackFn,
                           "oplog fetcher"),
      _metadataObject(makeMetadataObject(config.getProtocolVersion() == 1LL)),
      _requiredRBID(requiredRBID),
      _requireFresherSyncSource(requireFresherSyncSource),
      _dataReplicatorExternalState(dataReplicatorExternalState),
      _enqueueDocumentsFn(enqueueDocumentsFn),
      _awaitDataTimeout(calculateAwaitDataTimeout(config)),
      _batchSize(batchSize) {

    invariant(config.isInitialized());
    invariant(enqueueDocumentsFn);
}

OplogFetcher::~OplogFetcher() {
    shutdown();
    join();
}

BSONObj OplogFetcher::_makeFindCommandObject(const NamespaceString& nss,
                                             OpTime lastOpTimeFetched,
                                             Milliseconds findMaxTime) const {
    auto lastCommittedWithCurrentTerm =
        _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    auto term = lastCommittedWithCurrentTerm.value;
    BSONObjBuilder cmdBob;
    cmdBob.append("find", nss.coll());
    cmdBob.append("filter", BSON("ts" << BSON("$gte" << lastOpTimeFetched.getTimestamp())));
    cmdBob.append("tailable", true);
    cmdBob.append("oplogReplay", true);
    cmdBob.append("awaitData", true);
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(findMaxTime));
    cmdBob.append("batchSize", _batchSize);

    if (term != OpTime::kUninitializedTerm) {
        cmdBob.append("term", term);
    }

    cmdBob.append("readConcern", BSON("afterOpTime" << lastOpTimeFetched));

    return cmdBob.obj();
}

BSONObj OplogFetcher::_makeMetadataObject() const {
    return _metadataObject;
}

BSONObj OplogFetcher::getMetadataObject_forTest() const {
    return _metadataObject;
}

Milliseconds OplogFetcher::getAwaitDataTimeout_forTest() const {
    return _getGetMoreMaxTime();
}

Milliseconds OplogFetcher::_getGetMoreMaxTime() const {
    return _awaitDataTimeout;
}

StatusWith<BSONObj> OplogFetcher::_onSuccessfulBatch(const Fetcher::QueryResponse& queryResponse) {

    // Stop fetching and return on fail point.
    // This fail point makes the oplog fetcher ignore the downloaded batch of operations and not
    // error out. The FailPointEnabled error will be caught by the AbstractOplogFetcher.
    if (MONGO_FAIL_POINT(stopReplProducer)) {
        return Status(ErrorCodes::FailPointEnabled, "stopReplProducer fail point is enabled");
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

    auto oqMetadataResult = parseOplogQueryMetadata(queryResponse);
    if (!oqMetadataResult.isOK()) {
        error() << "invalid oplog query metadata from sync source " << _getSource() << ": "
                << oqMetadataResult.getStatus() << ": " << queryResponse.otherFields.metadata;
        return oqMetadataResult.getStatus();
    }
    auto oqMetadata = oqMetadataResult.getValue();

    // This lastFetched value is the last OpTime from the previous batch.
    auto lastFetched = _getLastOpTimeWithHashFetched();

    // Check start of remote oplog and, if necessary, stop fetcher to execute rollback.
    if (queryResponse.first) {
        auto remoteRBID = oqMetadata ? boost::make_optional(oqMetadata->getRBID()) : boost::none;
        auto remoteLastApplied =
            oqMetadata ? boost::make_optional(oqMetadata->getLastOpApplied()) : boost::none;
        auto status = checkRemoteOplogStart(documents,
                                            lastFetched,
                                            remoteLastApplied,
                                            _requiredRBID,
                                            remoteRBID,
                                            _requireFresherSyncSource);
        if (!status.isOK()) {
            // Stop oplog fetcher and execute rollback if necessary.
            return status;
        }

        LOG(1) << "oplog fetcher successfully fetched from " << _getSource();

        // If this is the first batch and no rollback is needed, skip the first document.
        firstDocToApply++;
    }

    auto validateResult = OplogFetcher::validateDocuments(
        documents, queryResponse.first, lastFetched.opTime.getTimestamp());
    if (!validateResult.isOK()) {
        return validateResult.getStatus();
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
            error() << "invalid replication metadata from sync source " << _getSource() << ": "
                    << metadataResult.getStatus() << ": " << metadataObj;
            return metadataResult.getStatus();
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
        return status;
    }

    if (_dataReplicatorExternalState->shouldStopFetching(
            _getSource(), replSetMetadata, oqMetadata)) {
        str::stream errMsg;
        errMsg << "sync source " << _getSource().toString();
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
        return Status(ErrorCodes::InvalidSyncSource, errMsg);
    }

    auto lastCommittedWithCurrentTerm =
        _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    return makeGetMoreCommandObject(queryResponse.nss,
                                    queryResponse.cursorId,
                                    lastCommittedWithCurrentTerm,
                                    _getGetMoreMaxTime(),
                                    _batchSize);
}
}  // namespace repl
}  // namespace mongo
