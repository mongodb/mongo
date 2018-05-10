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

#include "mongo/base/init.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_buffer_proxy.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

const char kCollectionOplogBufferName[] = "collection";
const char kBlockingQueueOplogBufferName[] = "inMemoryBlockingQueue";

// Set this to specify whether to use a collection to buffer the oplog on the destination server
// during initial sync to prevent rolling over the oplog.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(initialSyncOplogBuffer,
                                      std::string,
                                      kCollectionOplogBufferName);

// Set this to specify size of read ahead buffer in the OplogBufferCollection.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(initialSyncOplogBufferPeekCacheSize, int, 10000);

MONGO_INITIALIZER(initialSyncOplogBuffer)(InitializerContext*) {
    if ((initialSyncOplogBuffer != kCollectionOplogBufferName) &&
        (initialSyncOplogBuffer != kBlockingQueueOplogBufferName)) {
        return Status(ErrorCodes::BadValue,
                      "unsupported initial sync oplog buffer option: " + initialSyncOplogBuffer);
    }
    return Status::OK();
}

}  // namespace

DataReplicatorExternalStateImpl::DataReplicatorExternalStateImpl(
    ReplicationCoordinator* replicationCoordinator,
    ReplicationCoordinatorExternalState* replicationCoordinatorExternalState)
    : _replicationCoordinator(replicationCoordinator),
      _replicationCoordinatorExternalState(replicationCoordinatorExternalState) {}

executor::TaskExecutor* DataReplicatorExternalStateImpl::getTaskExecutor() const {
    return _replicationCoordinatorExternalState->getTaskExecutor();
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
    OperationContext* opCtx) const {
    if (initialSyncOplogBuffer == kCollectionOplogBufferName) {
        invariant(initialSyncOplogBufferPeekCacheSize >= 0);
        OplogBufferCollection::Options options;
        options.peekCacheSize = std::size_t(initialSyncOplogBufferPeekCacheSize);
        return stdx::make_unique<OplogBufferProxy>(
            stdx::make_unique<OplogBufferCollection>(StorageInterface::get(opCtx), options));
    } else {
        return stdx::make_unique<OplogBufferBlockingQueue>();
    }
}

StatusWith<OplogApplier::Operations> DataReplicatorExternalStateImpl::getNextApplierBatch(
    OperationContext* opCtx,
    OplogBuffer* oplogBuffer,
    const OplogApplier::BatchLimits& batchLimits) {
    OplogApplier oplogApplier(
        nullptr, oplogBuffer, nullptr, nullptr, nullptr, nullptr, {}, nullptr);
    return oplogApplier.getNextApplierBatch(opCtx, batchLimits);
}

StatusWith<ReplSetConfig> DataReplicatorExternalStateImpl::getCurrentConfig() const {
    return _replicationCoordinator->getConfig();
}

StatusWith<OpTime> DataReplicatorExternalStateImpl::_multiApply(OperationContext* opCtx,
                                                                MultiApplier::Operations ops,
                                                                OplogApplier::Observer* observer,
                                                                const HostAndPort& source,
                                                                ThreadPool* writerPool) {
    auto replicationProcess = ReplicationProcess::get(opCtx);
    auto consistencyMarkers = replicationProcess->getConsistencyMarkers();
    auto storageInterface = StorageInterface::get(opCtx);
    SyncTail syncTail(
        observer, consistencyMarkers, storageInterface, repl::multiInitialSyncApply, writerPool);
    syncTail.setHostname(source.toString());
    return syncTail.multiApply(opCtx, std::move(ops));
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
