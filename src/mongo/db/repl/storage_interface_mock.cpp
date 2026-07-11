// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/storage_interface_mock.h"

#include "mongo/logv2/log.h"

#include <iterator>
#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

StorageInterfaceMock::StorageInterfaceMock() = default;

StatusWith<int> StorageInterfaceMock::getRollbackID(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_rbidInitialized) {
        return Status(ErrorCodes::NamespaceNotFound, "Rollback ID not initialized");
    }
    return _rbid;
}

StatusWith<int> StorageInterfaceMock::initializeRollbackID(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_rbidInitialized) {
        return Status(ErrorCodes::NamespaceExists, "Rollback ID already initialized");
    }
    _rbidInitialized = true;

    // Start the mock RBID at a very high number to differentiate it from uninitialized RBIDs.
    _rbid = 100;
    return _rbid;
}

StatusWith<int> StorageInterfaceMock::incrementRollbackID(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_rbidInitialized) {
        return Status(ErrorCodes::NamespaceNotFound, "Rollback ID not initialized");
    }
    _rbid++;
    return _rbid;
}

void StorageInterfaceMock::setStableTimestamp(ServiceContext* serviceCtx,
                                              Timestamp snapshotName,
                                              bool force) {
    std::lock_guard<std::mutex> lock(_mutex);
    _stableTimestamp = snapshotName;
}

void StorageInterfaceMock::setInitialDataTimestamp(ServiceContext* serviceCtx,
                                                   Timestamp snapshotName) {
    std::lock_guard<std::mutex> lock(_mutex);
    _initialDataTimestamp = snapshotName;
}

Timestamp StorageInterfaceMock::getStableTimestamp() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _stableTimestamp;
}

Timestamp StorageInterfaceMock::getInitialDataTimestamp(ServiceContext* serviceCtx) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _initialDataTimestamp;
}

Timestamp StorageInterfaceMock::getAllDurableTimestamp(ServiceContext* serviceCtx) const {
    return allDurableTimestamp;
}

Status CollectionBulkLoaderMock::init(const std::vector<BSONObj>& secondaryIndexSpecs) {
    LOGV2_DEBUG(21757, 1, "CollectionBulkLoaderMock::init called");
    stats->initCalled = true;
    return Status::OK();
}

Status CollectionBulkLoaderMock::insertDocuments(std::span<BSONObj> docs,
                                                 ParseRecordIdAndDocFunc fn) {
    LOGV2_DEBUG(21758, 1, "CollectionBulkLoaderMock::insertDocuments called");
    auto status = insertDocsFn(docs, fn);

    // Only count if it succeeds.
    if (status.isOK()) {
        stats->insertCount += docs.size();
    }
    return status;
}

Status CollectionBulkLoaderMock::commit() {
    LOGV2_DEBUG(21759, 1, "CollectionBulkLoaderMock::commit called");
    stats->commitCalled = true;
    return commitFn();
}

}  // namespace repl
}  // namespace mongo
