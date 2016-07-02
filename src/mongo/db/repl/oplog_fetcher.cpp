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

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

Seconds OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout(2);

namespace {

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

OplogFetcher::OplogFetcher(executor::TaskExecutor* exec,
                           OpTimeWithHash lastFetched,
                           HostAndPort source,
                           NamespaceString oplogNSS,
                           ReplicaSetConfig config,
                           DataReplicatorExternalState* dataReplicatorExternalState,
                           EnqueueDocumentsFn enqueueDocumentsFn,
                           OnShutdownCallbackFn onShutdownCallbackFn)
    : _dataReplicatorExternalState(dataReplicatorExternalState),
      _fetcher(exec,
               source,
               oplogNSS.db().toString(),
               makeFindCommandObject(dataReplicatorExternalState, oplogNSS, lastFetched.opTime),
               stdx::bind(
                   &OplogFetcher::_callback, this, stdx::placeholders::_1, stdx::placeholders::_3),
               uassertStatusOK(makeMetadataObject(config.getProtocolVersion() == 1LL)),
               config.getElectionTimeoutPeriod()),
      _enqueueDocumentsFn(enqueueDocumentsFn),
      _awaitDataTimeout(calculateAwaitDataTimeout(config)),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(lastFetched) {
    uassert(ErrorCodes::BadValue, "null last optime fetched", !lastFetched.opTime.isNull());
    uassert(ErrorCodes::InvalidReplicaSetConfig,
            "uninitialized replica set configuration",
            config.isInitialized());
    uassert(ErrorCodes::BadValue, "null enqueueDocuments function", enqueueDocumentsFn);
    uassert(ErrorCodes::BadValue, "null onShutdownCallback function", onShutdownCallbackFn);
}

std::string OplogFetcher::toString() const {
    return str::stream() << "OplogReader -"
                         << " last optime fetched: " << _lastFetched.opTime.toString()
                         << " last hash fetched: " << _lastFetched.value
                         << " fetcher: " << _fetcher.getDiagnosticString();
}

bool OplogFetcher::isActive() const {
    return _fetcher.isActive();
}

Status OplogFetcher::startup() {
    return _fetcher.schedule();
}

void OplogFetcher::shutdown() {
    _fetcher.cancel();
}

void OplogFetcher::join() {
    _fetcher.wait();
}

OpTimeWithHash OplogFetcher::getLastOpTimeWithHashFetched() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _lastFetched;
}

BSONObj OplogFetcher::getCommandObject_forTest() const {
    return _fetcher.getCommandObject();
}

BSONObj OplogFetcher::getMetadataObject_forTest() const {
    return _fetcher.getMetadataObject();
}

Milliseconds OplogFetcher::getRemoteCommandTimeout_forTest() const {
    return _fetcher.getTimeout();
}

Milliseconds OplogFetcher::getAwaitDataTimeout_forTest() const {
    return _awaitDataTimeout;
}

void OplogFetcher::_callback(const Fetcher::QueryResponseStatus& result,
                             BSONObjBuilder* getMoreBob) {
    // if target cut connections between connecting and querying (for
    // example, because it stepped down) we might not have a cursor
    if (!result.isOK()) {
        LOG(2) << "Error returned from oplog query: " << result.getStatus();
        _onShutdown(result.getStatus());
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
            error() << "invalid replication metadata from sync source " << _fetcher.getSource()
                    << ": " << metadataResult.getStatus() << ": " << metadataObj;
            _onShutdown(metadataResult.getStatus());
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
            _onShutdown(status, opTimeWithHash);
            return;
        }

        // If this is the first batch and no rollback is needed, skip the first document.
        firstDocToApply++;
    }

    auto validateResult = OplogFetcher::validateDocuments(
        documents, queryResponse.first, opTimeWithHash.opTime.getTimestamp());
    if (!validateResult.isOK()) {
        _onShutdown(validateResult.getStatus(), opTimeWithHash);
        return;
    }
    auto info = validateResult.getValue();

    // TODO: back pressure handling will be added in SERVER-23499.
    _enqueueDocumentsFn(firstDocToApply, documents.cend(), info, queryResponse.elapsedMillis);

    // Update last fetched info.
    if (firstDocToApply != documents.cend()) {
        opTimeWithHash = info.lastDocument;
        LOG(3) << "batch resetting last fetched optime: " << opTimeWithHash.opTime
               << "; hash: " << opTimeWithHash.value;

        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _lastFetched = opTimeWithHash;
    }

    if (_dataReplicatorExternalState->shouldStopFetching(_fetcher.getSource(), metadata)) {
        _onShutdown(Status(ErrorCodes::InvalidSyncSource,
                           str::stream() << "sync source " << _fetcher.getSource().toString()
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
        _onShutdown(Status::OK(), opTimeWithHash);
        return;
    }

    getMoreBob->appendElements(makeGetMoreCommandObject(_dataReplicatorExternalState,
                                                        queryResponse.nss,
                                                        queryResponse.cursorId,
                                                        _awaitDataTimeout));
}

void OplogFetcher::_onShutdown(Status status) {
    _onShutdown(status, getLastOpTimeWithHashFetched());
}

void OplogFetcher::_onShutdown(Status status, OpTimeWithHash opTimeWithHash) {
    _onShutdownCallbackFn(status, opTimeWithHash);
}


}  // namespace repl
}  // namespace mongo
