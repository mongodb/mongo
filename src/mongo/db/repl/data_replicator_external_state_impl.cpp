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


#include <cstddef>
#include <string>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_buffer_proxy.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {
namespace {

// The maximum size of the oplog buffer is set to 256MB.
constexpr std::size_t kOplogBufferSize = 256 * 1024 * 1024;

const char kCollectionOplogBufferName[] = "collection";
const char kBlockingQueueOplogBufferName[] = "inMemoryBlockingQueue";

MONGO_INITIALIZER(initialSyncOplogBuffer)(InitializerContext*) {
    if ((initialSyncOplogBuffer != kCollectionOplogBufferName) &&
        (initialSyncOplogBuffer != kBlockingQueueOplogBufferName)) {
        uasserted(ErrorCodes::BadValue,
                  "unsupported initial sync oplog buffer option: " + initialSyncOplogBuffer);
    }
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

std::shared_ptr<executor::TaskExecutor> DataReplicatorExternalStateImpl::getSharedTaskExecutor()
    const {
    return _replicationCoordinatorExternalState->getSharedTaskExecutor();
}

OpTimeWithTerm DataReplicatorExternalStateImpl::getCurrentTermAndLastCommittedOpTime() {
    return {_replicationCoordinator->getTerm(), _replicationCoordinator->getLastCommittedOpTime()};
}

void DataReplicatorExternalStateImpl::processMetadata(const rpc::ReplSetMetadata& replMetadata,
                                                      const rpc::OplogQueryMetadata& oqMetadata) {
    OpTimeAndWallTime newCommitPoint = oqMetadata.getLastOpCommitted();

    const bool fromSyncSource = true;
    _replicationCoordinator->advanceCommitPoint(newCommitPoint, fromSyncSource);

    _replicationCoordinator->processReplSetMetadata(replMetadata);

    if (oqMetadata.hasPrimaryIndex()) {
        _replicationCoordinator->cancelAndRescheduleElectionTimeout();
    }
}

ChangeSyncSourceAction DataReplicatorExternalStateImpl::shouldStopFetching(
    const HostAndPort& source,
    const rpc::ReplSetMetadata& replMetadata,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {
    // Re-evaluate quality of sync target.
    auto changeSyncSourceAction = _replicationCoordinator->shouldChangeSyncSource(
        source, replMetadata, oqMetadata, previousOpTimeFetched, lastOpTimeFetched);
    if (changeSyncSourceAction != ChangeSyncSourceAction::kContinueSyncing) {
        LOGV2(21150,
              "Canceling oplog query due to OplogQueryMetadata. We have to choose a new "
              "sync source",
              "syncSource"_attr = source,
              "lastAppliedOpTime"_attr = oqMetadata.getLastOpApplied(),
              "syncSourceIndex"_attr = oqMetadata.getSyncSourceIndex());
    }
    return changeSyncSourceAction;
}

ChangeSyncSourceAction DataReplicatorExternalStateImpl::shouldStopFetchingOnError(
    const HostAndPort& source, const OpTime& lastOpTimeFetched) const {
    auto changeSyncSourceAction =
        _replicationCoordinator->shouldChangeSyncSourceOnError(source, lastOpTimeFetched);
    if (changeSyncSourceAction != ChangeSyncSourceAction::kContinueSyncing) {
        LOGV2(6341701,
              "Canceling oplog query on fetch error. We have to choose a new sync source",
              "syncSource"_attr = source);
    }
    return changeSyncSourceAction;
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateImpl::makeInitialSyncOplogBuffer(
    OperationContext* opCtx) const {
    if (initialSyncOplogBuffer == kCollectionOplogBufferName) {
        invariant(initialSyncOplogBufferPeekCacheSize >= 0);
        OplogBufferCollection::Options options;
        options.peekCacheSize = std::size_t(initialSyncOplogBufferPeekCacheSize);
        return std::make_unique<OplogBufferProxy>(
            std::make_unique<OplogBufferCollection>(StorageInterface::get(opCtx), options));
    } else {
        return std::make_unique<OplogBufferBlockingQueue>(kOplogBufferSize);
    }
}

std::unique_ptr<OplogApplier> DataReplicatorExternalStateImpl::makeOplogApplier(
    OplogBuffer* oplogBuffer,
    OplogApplier::Observer* observer,
    ReplicationConsistencyMarkers* consistencyMarkers,
    StorageInterface* storageInterface,
    const OplogApplier::Options& options,
    ThreadPool* writerPool) {
    return std::make_unique<OplogApplierImpl>(getTaskExecutor(),
                                              oplogBuffer,
                                              observer,
                                              _replicationCoordinator,
                                              consistencyMarkers,
                                              storageInterface,
                                              options,
                                              writerPool);
}

StatusWith<ReplSetConfig> DataReplicatorExternalStateImpl::getCurrentConfig() const {
    return _replicationCoordinator->getConfig();
}

StatusWith<BSONObj> DataReplicatorExternalStateImpl::loadLocalConfigDocument(
    OperationContext* opCtx) const {
    return _replicationCoordinatorExternalState->loadLocalConfigDocument(opCtx);
}

Status DataReplicatorExternalStateImpl::storeLocalConfigDocument(OperationContext* opCtx,
                                                                 const BSONObj& config) {
    return _replicationCoordinatorExternalState->storeLocalConfigDocument(
        opCtx, config, false /* write oplog entry */);
}

StatusWith<LastVote> DataReplicatorExternalStateImpl::loadLocalLastVoteDocument(
    OperationContext* opCtx) const {
    return _replicationCoordinatorExternalState->loadLocalLastVoteDocument(opCtx);
}

JournalListener* DataReplicatorExternalStateImpl::getReplicationJournalListener() {
    return _replicationCoordinatorExternalState->getReplicationJournalListener();
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
