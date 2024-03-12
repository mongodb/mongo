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

#include <iterator>
#include <mutex>

#include <boost/move/utility_core.hpp>

#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

StorageInterfaceMock::StorageInterfaceMock() = default;

StatusWith<int> StorageInterfaceMock::getRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lock(_mutex);
    if (!_rbidInitialized) {
        return Status(ErrorCodes::NamespaceNotFound, "Rollback ID not initialized");
    }
    return _rbid;
}

StatusWith<int> StorageInterfaceMock::initializeRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lock(_mutex);
    if (_rbidInitialized) {
        return Status(ErrorCodes::NamespaceExists, "Rollback ID already initialized");
    }
    _rbidInitialized = true;

    // Start the mock RBID at a very high number to differentiate it from uninitialized RBIDs.
    _rbid = 100;
    return _rbid;
}

StatusWith<int> StorageInterfaceMock::incrementRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lock(_mutex);
    if (!_rbidInitialized) {
        return Status(ErrorCodes::NamespaceNotFound, "Rollback ID not initialized");
    }
    _rbid++;
    return _rbid;
}

void StorageInterfaceMock::setStableTimestamp(ServiceContext* serviceCtx,
                                              Timestamp snapshotName,
                                              bool force) {
    stdx::lock_guard<Latch> lock(_mutex);
    _stableTimestamp = snapshotName;
}

void StorageInterfaceMock::setInitialDataTimestamp(ServiceContext* serviceCtx,
                                                   Timestamp snapshotName) {
    stdx::lock_guard<Latch> lock(_mutex);
    _initialDataTimestamp = snapshotName;
}

Timestamp StorageInterfaceMock::getStableTimestamp() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _stableTimestamp;
}

Timestamp StorageInterfaceMock::getInitialDataTimestamp(ServiceContext* serviceCtx) const {
    stdx::lock_guard<Latch> lock(_mutex);
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

Status CollectionBulkLoaderMock::insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                                                 const std::vector<BSONObj>::const_iterator end,
                                                 ParseRecordIdAndDocFunc fn) {
    LOGV2_DEBUG(21758, 1, "CollectionBulkLoaderMock::insertDocuments called");
    auto status = insertDocsFn(begin, end, fn);

    // Only count if it succeeds.
    if (status.isOK()) {
        stats->insertCount += std::distance(begin, end);
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
