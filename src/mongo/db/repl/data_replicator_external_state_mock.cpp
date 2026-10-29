// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/data_replicator_external_state_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"

#include <memory>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

namespace {

constexpr std::size_t kTestOplogBufferSize = 64 * 1024 * 1024;
constexpr std::size_t kTestOplogBufferCount = std::numeric_limits<std::size_t>::max();

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

private:
    void _run(OplogBuffer* oplogBuffer) final {}
    StatusWith<OpTime> _applyOplogBatch(OperationContext* opCtx,
                                        std::vector<OplogEntry> ops) final {
        return _externalState->applyOplogBatchFn(opCtx, ops, _observer);
    }

    OplogApplier::Observer* const _observer;
    DataReplicatorExternalStateMock* const _externalState;
};

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
    std::lock_guard<std::mutex> lk(_mutex);
    return {currentTerm, lastCommittedOpTime};
}

void DataReplicatorExternalStateMock::processMetadata(const rpc::ReplSetMetadata& replMetadata,
                                                      const rpc::OplogQueryMetadata& oqMetadata) {
    std::lock_guard<std::mutex> lk(_mutex);
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
    std::lock_guard<std::mutex> lk(_mutex);
    lastSyncSourceChecked = source;
    syncSourceLastOpTime = oqMetadata.getLastOpApplied();
    syncSourceHasSyncSource = oqMetadata.getSyncSourceIndex() != -1;
    return shouldStopFetchingResult;
}

ChangeSyncSourceAction DataReplicatorExternalStateMock::shouldStopFetchingOnError(
    const HostAndPort& source, const OpTime& lastOpTimeFetched) const {
    std::lock_guard<std::mutex> lk(_mutex);
    lastSyncSourceChecked = source;
    return shouldStopFetchingResult;
}

std::unique_ptr<OplogBuffer> DataReplicatorExternalStateMock::makeInitialSyncOplogBuffer(
    OperationContext* opCtx) const {
    return std::make_unique<OplogBufferBlockingQueue>(kTestOplogBufferSize, kTestOplogBufferCount);
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
    std::lock_guard<std::mutex> lk(_mutex);
    return replSetConfigResult;
}

StatusWith<BSONObj> DataReplicatorExternalStateMock::loadLocalConfigDocument(
    OperationContext* opCtx) const {
    std::lock_guard<std::mutex> lk(_mutex);
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
