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

#include <memory>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"

namespace mongo {
namespace repl {

namespace {

constexpr std::size_t kTestOplogBufferSize = 64 * 1024 * 1024;

class OplogApplierMock : public OplogApplier {
    OplogApplierMock(const OplogApplierMock&) = delete;
    OplogApplierMock& operator=(const OplogApplierMock&) = delete;

public:
    OplogApplierMock(executor::TaskExecutor* executor,
                     OplogBuffer* oplogBuffer,
                     Observer* observer,
                     DataReplicatorExternalStateMock* externalState)
        : OplogApplier(executor,
                       oplogBuffer,
                       observer,
                       OplogApplier::Options(OplogApplication::Mode::kSecondary)),
          _observer(observer),
          _externalState(externalState) {}

    void scheduleWritesToOplogAndChangeCollection(OperationContext* opCtx,
                                                  StorageInterface* storageInterface,
                                                  ThreadPool* writerPool,
                                                  const std::vector<OplogEntry>& ops,
                                                  bool skipWritesToOplog) override;

private:
    void _run(OplogBuffer* oplogBuffer) final {}
    StatusWith<OpTime> _applyOplogBatch(OperationContext* opCtx,
                                        std::vector<OplogEntry> ops) final {
        return _externalState->applyOplogBatchFn(opCtx, ops, _observer);
    }

    OplogApplier::Observer* const _observer;
    DataReplicatorExternalStateMock* const _externalState;
};

void OplogApplierMock::scheduleWritesToOplogAndChangeCollection(OperationContext* opCtx,
                                                                StorageInterface* storageInterface,
                                                                ThreadPool* writerPool,
                                                                const std::vector<OplogEntry>& ops,
                                                                bool skipWritesToOplog) {}

}  // namespace

DataReplicatorExternalStateMock::DataReplicatorExternalStateMock()
    : applyOplogBatchFn([](OperationContext*,
                           const std::vector<OplogEntry>& ops,
                           OplogApplier::Observer*) { return ops.back().getOpTime(); }) {}

executor::TaskExecutor* DataReplicatorExternalStateMock::getTaskExecutor() const {
    return taskExecutor.get();
}
std::shared_ptr<executor::TaskExecutor> DataReplicatorExternalStateMock::getSharedTaskExecutor()
    const {
    return taskExecutor;
}

OpTimeWithTerm DataReplicatorExternalStateMock::getCurrentTermAndLastCommittedOpTime() {
    stdx::lock_guard<Latch> lk(_mutex);
    return {currentTerm, lastCommittedOpTime};
}

void DataReplicatorExternalStateMock::processMetadata(const rpc::ReplSetMetadata& replMetadata,
                                                      const rpc::OplogQueryMetadata& oqMetadata) {
    stdx::lock_guard<Latch> lk(_mutex);
    replMetadataProcessed = rpc::ReplSetMetadata(replMetadata);
    oqMetadataProcessed = rpc::OplogQueryMetadata(oqMetadata);
    metadataWasProcessed = true;
}

ChangeSyncSourceAction DataReplicatorExternalStateMock::shouldStopFetching(
    const HostAndPort& source,
    const rpc::ReplSetMetadata& replMetadata,
    const rpc::OplogQueryMetadata& oqMetadata,
    const OpTime& previousOpTimeFetched,
    const OpTime& lastOpTimeFetched) const {
    stdx::lock_guard<Latch> lk(_mutex);
    lastSyncSourceChecked = source;
    syncSourceLastOpTime = oqMetadata.getLastOpApplied();
    syncSourceHasSyncSource = oqMetadata.getSyncSourceIndex() != -1;
    return shouldStopFetchingResult;
}

ChangeSyncSourceAction DataReplicatorExternalStateMock::shouldStopFetchingOnError(
    const HostAndPort& source, const OpTime& lastOpTimeFetched) const {
    stdx::lock_guard<Latch> lk(_mutex);
    lastSyncSourceChecked = source;
    return shouldStopFetchingResult;
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateMock::makeInitialSyncOplogBuffer(
    OperationContext* opCtx) const {
    return std::make_unique<OplogBufferBlockingQueue>(kTestOplogBufferSize);
}

std::unique_ptr<OplogApplier> DataReplicatorExternalStateMock::makeOplogApplier(
    OplogBuffer* oplogBuffer,
    OplogApplier::Observer* observer,
    ReplicationConsistencyMarkers*,
    StorageInterface*,
    const OplogApplier::Options&,
    ThreadPool*) {
    return std::make_unique<OplogApplierMock>(getTaskExecutor(), oplogBuffer, observer, this);
}

StatusWith<ReplSetConfig> DataReplicatorExternalStateMock::getCurrentConfig() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return replSetConfigResult;
}

StatusWith<BSONObj> DataReplicatorExternalStateMock::loadLocalConfigDocument(
    OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (replSetConfigResult.isOK()) {
        return replSetConfigResult.getValue().toBSON();
    }
    return replSetConfigResult.getStatus();
}

Status DataReplicatorExternalStateMock::storeLocalConfigDocument(OperationContext* opCtx,
                                                                 const BSONObj& config) {
    return Status::OK();
}

JournalListener* DataReplicatorExternalStateMock::getReplicationJournalListener() {
    return nullptr;
}

StatusWith<LastVote> DataReplicatorExternalStateMock::loadLocalLastVoteDocument(
    OperationContext* opCtx) const {
    return StatusWith<LastVote>(ErrorCodes::NoMatchingDocument, "mock");
}

}  // namespace repl
}  // namespace mongo
