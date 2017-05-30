/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rollback_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {


namespace {

const auto getReplicationProcess =
    ServiceContext::declareDecoration<std::unique_ptr<ReplicationProcess>>();

const int kUninitializedRollbackId = -1;

const auto kRollbackNamespacePrefix = "local.system.rollback."_sd;

const auto kRollbackProgressIdDoc = BSON("_id"
                                         << "rollbackProgress");
const auto kRollbackProgressIdKey = kRollbackProgressIdDoc["_id"];
}  // namespace

const NamespaceString ReplicationProcess::kRollbackProgressNamespace(kRollbackNamespacePrefix +
                                                                     "progress");

ReplicationProcess* ReplicationProcess::get(ServiceContext* service) {
    return getReplicationProcess(service).get();
}

ReplicationProcess* ReplicationProcess::get(ServiceContext& service) {
    return getReplicationProcess(service).get();
}

ReplicationProcess* ReplicationProcess::get(OperationContext* opCtx) {
    return get(opCtx->getClient()->getServiceContext());
}


void ReplicationProcess::set(ServiceContext* service, std::unique_ptr<ReplicationProcess> process) {
    auto& replicationProcess = getReplicationProcess(service);
    replicationProcess = std::move(process);
}

ReplicationProcess::ReplicationProcess(
    StorageInterface* storageInterface,
    std::unique_ptr<ReplicationConsistencyMarkers> consistencyMarkers)
    : _storageInterface(storageInterface),
      _consistencyMarkers(std::move(consistencyMarkers)),
      _rbid(kUninitializedRollbackId) {}

StatusWith<int> ReplicationProcess::getRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (kUninitializedRollbackId != _rbid) {
        return _rbid;
    }

    // The _rbid, which caches the rollback ID persisted in the local.system.rollback.id collection,
    // may be uninitialized for a couple of reasons:
    // 1) This is the first time we are retrieving the rollback ID; or
    // 2) The rollback ID was incremented previously using this class which has the side-effect of
    //    invalidating the cached value.
    auto rbidResult = _storageInterface->getRollbackID(opCtx);
    if (!rbidResult.isOK()) {
        return rbidResult;
    }
    _rbid = rbidResult.getValue();

    invariant(kUninitializedRollbackId != _rbid);
    return _rbid;
}

Status ReplicationProcess::initializeRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    invariant(kUninitializedRollbackId == _rbid);

    // Do not make any assumptions about the starting value of the rollback ID in the
    // local.system.rollback.id collection other than it cannot be "kUninitializedRollbackId".
    // Leave _rbid uninitialized until the next getRollbackID() to retrieve the actual value
    // from storage.

    return _storageInterface->initializeRollbackID(opCtx);
}

Status ReplicationProcess::incrementRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto status = _storageInterface->incrementRollbackID(opCtx);

    // If the rollback ID was incremented successfully, reset _rbid so that we will read from
    // storage next time getRollbackID() is called.
    if (status.isOK()) {
        _rbid = kUninitializedRollbackId;
    }

    return status;
}

StatusWith<OpTime> ReplicationProcess::getRollbackProgress(OperationContext* opCtx) {
    auto documentResult =
        _storageInterface->findById(opCtx, kRollbackProgressNamespace, kRollbackProgressIdKey);
    if (!documentResult.isOK()) {
        return documentResult.getStatus();
    }
    const auto& doc = documentResult.getValue();
    RollbackProgress rollbackProgress;
    try {
        rollbackProgress = RollbackProgress::parse(IDLParserErrorContext("RollbackProgress"), doc);
    } catch (...) {
        return exceptionToStatus();
    }
    return rollbackProgress.getApplyUntil();
}

Status ReplicationProcess::setRollbackProgress(OperationContext* opCtx, const OpTime& applyUntil) {
    auto status = _storageInterface->createCollection(opCtx, kRollbackProgressNamespace, {});
    if (ErrorCodes::NamespaceExists == status.code()) {
        // Collection exists. Proceed to upsert progress document.
    } else if (!status.isOK()) {
        return status;
    }
    RollbackProgress rollbackProgress;
    rollbackProgress.set_id(kRollbackProgressIdKey.String());
    rollbackProgress.setApplyUntil(applyUntil);
    BSONObjBuilder bob;
    rollbackProgress.serialize(&bob);
    return _storageInterface->upsertById(
        opCtx, kRollbackProgressNamespace, kRollbackProgressIdKey, bob.obj());
}

Status ReplicationProcess::clearRollbackProgress(OperationContext* opCtx) {
    auto status = _storageInterface->deleteByFilter(opCtx, kRollbackProgressNamespace, {});
    if (ErrorCodes::NamespaceNotFound == status) {
        return Status::OK();
    } else if (!status.isOK()) {
        return status;
    }
    return Status::OK();
}

ReplicationConsistencyMarkers* ReplicationProcess::getConsistencyMarkers() {
    return _consistencyMarkers.get();
}

}  // namespace repl
}  // namespace mongo
