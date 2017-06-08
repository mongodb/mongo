/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <numeric>

#include "mongo/platform/basic.h"

#include "mongo/db/repl/storage_interface_mock.h"

#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

StatusWith<int> StorageInterfaceMock::getRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_rbidMutex);
    if (!_rbidInitialized) {
        return Status(ErrorCodes::NamespaceNotFound, "Rollback ID not initialized");
    }
    return _rbid;
}

Status StorageInterfaceMock::initializeRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_rbidMutex);
    if (_rbidInitialized) {
        return Status(ErrorCodes::NamespaceExists, "Rollback ID already initialized");
    }
    _rbidInitialized = true;

    // Start the mock RBID at a very high number to differentiate it from uninitialized RBIDs.
    _rbid = 100;
    return Status::OK();
}

Status StorageInterfaceMock::incrementRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_rbidMutex);
    if (!_rbidInitialized) {
        return Status(ErrorCodes::NamespaceNotFound, "Rollback ID not initialized");
    }
    _rbid++;
    return Status::OK();
}
Status CollectionBulkLoaderMock::init(const std::vector<BSONObj>& secondaryIndexSpecs) {
    LOG(1) << "CollectionBulkLoaderMock::init called";
    stats->initCalled = true;
    return Status::OK();
};

Status CollectionBulkLoaderMock::insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                                                 const std::vector<BSONObj>::const_iterator end) {
    LOG(1) << "CollectionBulkLoaderMock::insertDocuments called";
    const auto status = insertDocsFn(begin, end);

    // Only count if it succeeds.
    if (status.isOK()) {
        stats->insertCount += std::distance(begin, end);
    }
    return status;
};

Status CollectionBulkLoaderMock::commit() {
    LOG(1) << "CollectionBulkLoaderMock::commit called";
    stats->commitCalled = true;
    return commitFn();
};

}  // namespace repl
}  // namespace mongo
