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


#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_external_state_mock.h"

#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/sequence_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

ReplicationCoordinatorExternalStateMock::ReplicationCoordinatorExternalStateMock()
    : _localRsConfigDocument(ErrorCodes::NoMatchingDocument, "No local config document"),
      _localRsLastVoteDocument(ErrorCodes::NoMatchingDocument, "No local lastVote document"),
      _lastOpTime(ErrorCodes::NoMatchingDocument, "No last oplog entry"),
      _lastWallTime(ErrorCodes::NoMatchingDocument, "No last oplog entry"),
      _canAcquireGlobalSharedLock(true),
      _storeLocalConfigDocumentStatus(Status::OK()),
      _storeLocalLastVoteDocumentStatus(Status::OK()),
      _storeLocalLastVoteDocumentShouldHang(false),
      _connectionsClosed(false),
      _threadsStarted(false) {}

ReplicationCoordinatorExternalStateMock::~ReplicationCoordinatorExternalStateMock() {}

void ReplicationCoordinatorExternalStateMock::startThreads() {
    _threadsStarted = true;
}

bool ReplicationCoordinatorExternalStateMock::isInitialSyncFlagSet(OperationContext*) {
    return false;
}

void ReplicationCoordinatorExternalStateMock::startSteadyStateReplication(OperationContext*,
                                                                          ReplicationCoordinator*) {
}

Status ReplicationCoordinatorExternalStateMock::initializeReplSetStorage(OperationContext* opCtx,
                                                                         const BSONObj& config) {
    return storeLocalConfigDocument(opCtx, config, false);
}

void ReplicationCoordinatorExternalStateMock::shutdown(OperationContext*) {}

executor::TaskExecutor* ReplicationCoordinatorExternalStateMock::getTaskExecutor() const {
    return nullptr;
}

std::shared_ptr<executor::TaskExecutor>
ReplicationCoordinatorExternalStateMock::getSharedTaskExecutor() const {
    return nullptr;
}

ThreadPool* ReplicationCoordinatorExternalStateMock::getDbWorkThreadPool() const {
    return nullptr;
}

void ReplicationCoordinatorExternalStateMock::forwardSecondaryProgress() {}

bool ReplicationCoordinatorExternalStateMock::isSelf(const HostAndPort& host,
                                                     ServiceContext* const service) {
    return sequenceContains(_selfHosts, host);
}

void ReplicationCoordinatorExternalStateMock::addSelf(const HostAndPort& host) {
    _selfHosts.push_back(host);
}

void ReplicationCoordinatorExternalStateMock::clearSelfHosts() {
    _selfHosts.clear();
}

HostAndPort ReplicationCoordinatorExternalStateMock::getClientHostAndPort(
    const OperationContext* opCtx) {
    return _clientHostAndPort;
}

void ReplicationCoordinatorExternalStateMock::setClientHostAndPort(
    const HostAndPort& clientHostAndPort) {
    _clientHostAndPort = clientHostAndPort;
}

StatusWith<BSONObj> ReplicationCoordinatorExternalStateMock::loadLocalConfigDocument(
    OperationContext* opCtx) {
    return _localRsConfigDocument;
}

Status ReplicationCoordinatorExternalStateMock::storeLocalConfigDocument(OperationContext* opCtx,
                                                                         const BSONObj& config,
                                                                         bool writeOplog) {
    if (_storeLocalConfigDocumentStatus.isOK()) {
        setLocalConfigDocument(StatusWith<BSONObj>(config));
        return Status::OK();
    }
    return _storeLocalConfigDocumentStatus;
}

Status ReplicationCoordinatorExternalStateMock::replaceLocalConfigDocument(OperationContext* opCtx,
                                                                           const BSONObj& config) {
    return storeLocalConfigDocument(opCtx, config, false);
}

void ReplicationCoordinatorExternalStateMock::setLocalConfigDocument(
    const StatusWith<BSONObj>& localConfigDocument) {
    _localRsConfigDocument = localConfigDocument;
}

Status ReplicationCoordinatorExternalStateMock::createLocalLastVoteCollection(
    OperationContext* opCtx) {
    if (!_localRsLastVoteDocument.isOK()) {
        setLocalLastVoteDocument(LastVote{OpTime::kInitialTerm, -1});
    }
    return Status::OK();
}

StatusWith<LastVote> ReplicationCoordinatorExternalStateMock::loadLocalLastVoteDocument(
    OperationContext* opCtx) {
    return _localRsLastVoteDocument;
}

Status ReplicationCoordinatorExternalStateMock::storeLocalLastVoteDocument(
    OperationContext* opCtx, const LastVote& lastVote) {
    {
        stdx::unique_lock<Latch> lock(_shouldHangLastVoteMutex);
        while (_storeLocalLastVoteDocumentShouldHang) {
            _shouldHangLastVoteCondVar.wait(lock);
        }
    }
    if (_storeLocalLastVoteDocumentStatus.isOK()) {
        setLocalLastVoteDocument(StatusWith<LastVote>(lastVote));
        return Status::OK();
    }
    return _storeLocalLastVoteDocumentStatus;
}

void ReplicationCoordinatorExternalStateMock::setLocalLastVoteDocument(
    const StatusWith<LastVote>& localLastVoteDocument) {
    _localRsLastVoteDocument = localLastVoteDocument;
}

void ReplicationCoordinatorExternalStateMock::setGlobalTimestamp(ServiceContext* service,
                                                                 const Timestamp& newTime) {
    if (newTime > _globalTimestamp) {
        _globalTimestamp = newTime;
    }
}

Timestamp ReplicationCoordinatorExternalStateMock::getGlobalTimestamp(ServiceContext* service) {
    return _globalTimestamp;
}

bool ReplicationCoordinatorExternalStateMock::oplogExists(OperationContext* opCtx) {
    return true;
}

StatusWith<OpTimeAndWallTime> ReplicationCoordinatorExternalStateMock::loadLastOpTimeAndWallTime(
    OperationContext* opCtx) {
    if (_lastOpTime.getStatus().isOK()) {
        if (_lastWallTime.getStatus().isOK()) {
            OpTimeAndWallTime result = {_lastOpTime.getValue(), _lastWallTime.getValue()};
            return result;
        } else {
            return _lastWallTime.getStatus();
        }
    } else {
        return _lastOpTime.getStatus();
    }
}

void ReplicationCoordinatorExternalStateMock::setLastOpTimeAndWallTime(
    const StatusWith<OpTime>& lastApplied, Date_t lastAppliedWall) {
    _lastOpTime = lastApplied;
    _lastWallTime = lastAppliedWall;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalConfigDocumentStatus(Status status) {
    _storeLocalConfigDocumentStatus = status;
}

bool ReplicationCoordinatorExternalStateMock::threadsStarted() const {
    return _threadsStarted;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalLastVoteDocumentStatus(Status status) {
    _storeLocalLastVoteDocumentStatus = status;
}

void ReplicationCoordinatorExternalStateMock::setStoreLocalLastVoteDocumentToHang(bool hang) {
    stdx::unique_lock<Latch> lock(_shouldHangLastVoteMutex);
    _storeLocalLastVoteDocumentShouldHang = hang;
    if (!hang) {
        _shouldHangLastVoteCondVar.notify_all();
    }
}

void ReplicationCoordinatorExternalStateMock::setFirstOpTimeOfMyTerm(const OpTime& opTime) {
    _firstOpTimeOfMyTerm = opTime;
}

void ReplicationCoordinatorExternalStateMock::closeConnections() {
    _connectionsClosed = true;
}

void ReplicationCoordinatorExternalStateMock::onStepDownHook() {}

void ReplicationCoordinatorExternalStateMock::signalApplierToChooseNewSyncSource() {}

void ReplicationCoordinatorExternalStateMock::stopProducer() {}

void ReplicationCoordinatorExternalStateMock::startProducerIfStopped() {}

bool ReplicationCoordinatorExternalStateMock::tooStale() {
    return false;
}

void ReplicationCoordinatorExternalStateMock::clearCommittedSnapshot() {}

void ReplicationCoordinatorExternalStateMock::updateCommittedSnapshot(
    const OpTime& newCommitPoint) {}

void ReplicationCoordinatorExternalStateMock::updateLastAppliedSnapshot(const OpTime& optime) {}

bool ReplicationCoordinatorExternalStateMock::snapshotsEnabled() const {
    return _areSnapshotsEnabled;
}

void ReplicationCoordinatorExternalStateMock::setAreSnapshotsEnabled(bool val) {
    _areSnapshotsEnabled = val;
}

void ReplicationCoordinatorExternalStateMock::setElectionTimeoutOffsetLimitFraction(double val) {
    _electionTimeoutOffsetLimitFraction = val;
}

void ReplicationCoordinatorExternalStateMock::notifyOplogMetadataWaiters(
    const OpTime& committedOpTime) {}

boost::optional<OpTime> ReplicationCoordinatorExternalStateMock::getEarliestDropPendingOpTime()
    const {
    return {};
}

double ReplicationCoordinatorExternalStateMock::getElectionTimeoutOffsetLimitFraction() const {
    return _electionTimeoutOffsetLimitFraction;
}

bool ReplicationCoordinatorExternalStateMock::isReadCommittedSupportedByStorageEngine(
    OperationContext* opCtx) const {
    return _isReadCommittedSupported;
}

bool ReplicationCoordinatorExternalStateMock::isReadConcernSnapshotSupportedByStorageEngine(
    OperationContext* opCtx) const {
    return true;
}

std::size_t ReplicationCoordinatorExternalStateMock::getOplogFetcherSteadyStateMaxFetcherRestarts()
    const {
    return 0;
}

std::size_t ReplicationCoordinatorExternalStateMock::getOplogFetcherInitialSyncMaxFetcherRestarts()
    const {
    return 0;
}

void ReplicationCoordinatorExternalStateMock::setIsReadCommittedEnabled(bool val) {
    _isReadCommittedSupported = val;
}

void ReplicationCoordinatorExternalStateMock::onDrainComplete(OperationContext* opCtx) {}

OpTime ReplicationCoordinatorExternalStateMock::onTransitionToPrimary(OperationContext* opCtx) {
    _lastOpTime = _firstOpTimeOfMyTerm;
    _firstOpTimeOfMyTerm = OpTime();
    return fassert(40297, _lastOpTime);
}

void ReplicationCoordinatorExternalStateMock::startNoopWriter(OpTime opTime) {}

void ReplicationCoordinatorExternalStateMock::stopNoopWriter() {}

void ReplicationCoordinatorExternalStateMock::setupNoopWriter(Seconds waitTime) {}

bool ReplicationCoordinatorExternalStateMock::isCWWCSetOnConfigShard(
    OperationContext* opCtx) const {
    return true;
}

bool ReplicationCoordinatorExternalStateMock::isShardPartOfShardedCluster(
    OperationContext* opCtx) const {
    return true;
}

JournalListener* ReplicationCoordinatorExternalStateMock::getReplicationJournalListener() {
    MONGO_UNREACHABLE;
}

void ReplicationCoordinatorExternalStateMock::notifyOtherMemberDataChanged() {
    _otherMemberDataChanged = true;
}

void ReplicationCoordinatorExternalStateMock::clearOtherMemberDataChanged() {
    _otherMemberDataChanged = false;
}

bool ReplicationCoordinatorExternalStateMock::getOtherMemberDataChanged() const {
    return _otherMemberDataChanged;
}

}  // namespace repl
}  // namespace mongo
