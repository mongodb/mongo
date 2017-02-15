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

#include "mongo/db/repl/data_replicator_external_state_impl.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

DataReplicatorExternalStateImpl::DataReplicatorExternalStateImpl(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState)
    : _replicationCoordinator(replicationCoordinator),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState) {}

executor::TaskExecutor* DataReplicatorExternalStateImpl::getTaskExecutor() const {
    return _replicationCoordinatorExternalState->getTaskExecutor();
}

OldThreadPool* DataReplicatorExternalStateImpl::getDbWorkThreadPool() const {
    return _replicationCoordinatorExternalState->getDbWorkThreadPool();
}

OpTimeWithTerm DataReplicatorExternalStateImpl::getCurrentTermAndLastCommittedOpTime() {
    if (!_replicationCoordinator->isV1ElectionProtocol()) {
        return {OpTime::kUninitializedTerm, OpTime()};
    }
    return {_replicationCoordinator->getTerm(), _replicationCoordinator->getLastCommittedOpTime()};
}

void DataReplicatorExternalStateImpl::processMetadata(
    const rpc::ReplSetMetadata& replMetadata, boost::optional<rpc::OplogQueryMetadata> oqMetadata) {
    OpTime newCommitPoint;
    // If OplogQueryMetadata was provided, use its values, otherwise use the ones in
    // ReplSetMetadata.
    if (oqMetadata) {
        newCommitPoint = oqMetadata->getLastOpCommitted();
    } else {
        newCommitPoint = replMetadata.getLastOpCommitted();
    }
    _replicationCoordinator->advanceCommitPoint(newCommitPoint);

    _replicationCoordinator->processReplSetMetadata(replMetadata);

    if ((oqMetadata && (oqMetadata->getPrimaryIndex() != rpc::OplogQueryMetadata::kNoPrimary)) ||
        (replMetadata.getPrimaryIndex() != rpc::ReplSetMetadata::kNoPrimary)) {
        _replicationCoordinator->cancelAndRescheduleElectionTimeout();
    }
}

bool DataReplicatorExternalStateImpl::shouldStopFetching(
    const HostAndPort& source,
    const rpc::ReplSetMetadata& replMetadata,
    boost::optional<rpc::OplogQueryMetadata> oqMetadata) {
    // Re-evaluate quality of sync target.
    if (_replicationCoordinator->shouldChangeSyncSource(source, replMetadata, oqMetadata)) {
        // If OplogQueryMetadata was provided, its values were used to determine if we should
        // change sync sources.
        if (oqMetadata) {
            log() << "Canceling oplog query due to OplogQueryMetadata. We have to choose a new "
                     "sync source. Current source: "
                  << source << ", OpTime " << oqMetadata->getLastOpApplied()
                  << ", its sync source index:" << oqMetadata->getSyncSourceIndex();

        } else {
            log() << "Canceling oplog query due to ReplSetMetadata. We have to choose a new sync "
                     "source. Current source: "
                  << source << ", OpTime " << replMetadata.getLastOpVisible()
                  << ", its sync source index:" << replMetadata.getSyncSourceIndex();
        }
        return true;
    }
    return false;
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateImpl::makeInitialSyncOplogBuffer(
    OperationContext* txn) const {
    return _replicationCoordinatorExternalState->makeInitialSyncOplogBuffer(txn);
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateImpl::makeSteadyStateOplogBuffer(
    OperationContext* txn) const {
    return _replicationCoordinatorExternalState->makeSteadyStateOplogBuffer(txn);
}

StatusWith<ReplicaSetConfig> DataReplicatorExternalStateImpl::getCurrentConfig() const {
    return _replicationCoordinator->getConfig();
}

StatusWith<OpTime> DataReplicatorExternalStateImpl::_multiApply(
    OperationContext* txn,
    MultiApplier::Operations ops,
    MultiApplier::ApplyOperationFn applyOperation) {
    return _replicationCoordinatorExternalState->multiApply(txn, std::move(ops), applyOperation);
}

Status DataReplicatorExternalStateImpl::_multiSyncApply(MultiApplier::OperationPtrs* ops) {
    return _replicationCoordinatorExternalState->multiSyncApply(ops);
}

Status DataReplicatorExternalStateImpl::_multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                                                               const HostAndPort& source,
                                                               AtomicUInt32* fetchCount) {
    return _replicationCoordinatorExternalState->multiInitialSyncApply(ops, source, fetchCount);
}

ReplicationCoordinator* DataReplicatorExternalStateImpl::getReplicationCoordinator() const {
    return _replicationCoordinator;
}

ReplicationCoordinatorExternalState*
DataReplicatorExternalStateImpl::getReplicationCoordinatorExternalState() const {
    return _replicationCoordinatorExternalState;
}

}  // namespace repl
}  // namespace mongo
