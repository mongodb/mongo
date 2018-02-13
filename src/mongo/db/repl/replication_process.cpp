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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

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
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {


namespace {

const auto getReplicationProcess =
    ServiceContext::declareDecoration<std::unique_ptr<ReplicationProcess>>();

}  // namespace

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
    std::unique_ptr<ReplicationConsistencyMarkers> consistencyMarkers,
    std::unique_ptr<ReplicationRecovery> recovery)
    : _storageInterface(storageInterface),
      _consistencyMarkers(std::move(consistencyMarkers)),
      _recovery(std::move(recovery)),
      _rbid(kUninitializedRollbackId) {}

Status ReplicationProcess::refreshRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto rbidResult = _storageInterface->getRollbackID(opCtx);
    if (!rbidResult.isOK()) {
        return rbidResult.getStatus();
    }

    if (kUninitializedRollbackId == _rbid) {
        log() << "Rollback ID is " << rbidResult.getValue();
    } else {
        log() << "Rollback ID is " << rbidResult.getValue() << " (previously " << _rbid << ")";
    }
    _rbid = rbidResult.getValue();

    return Status::OK();
}

int ReplicationProcess::getRollbackID() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (kUninitializedRollbackId == _rbid) {
        // This may happen when serverStatus is called by an internal client before we have a chance
        // to read the rollback ID from storage.
        warning() << "Rollback ID is not initialized yet.";
    }
    return _rbid;
}

Status ReplicationProcess::initializeRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    invariant(kUninitializedRollbackId == _rbid);

    // Do not make any assumptions about the starting value of the rollback ID in the
    // local.system.rollback.id collection other than it cannot be "kUninitializedRollbackId".
    // Cache the rollback ID in _rbid to be returned the next time getRollbackID() is called.

    auto initRbidSW = _storageInterface->initializeRollbackID(opCtx);
    if (initRbidSW.isOK()) {
        log() << "Initialized the rollback ID to " << initRbidSW.getValue();
        _rbid = initRbidSW.getValue();
        invariant(kUninitializedRollbackId != _rbid);
    } else {
        warning() << "Failed to initialize the rollback ID: " << initRbidSW.getStatus().reason();
    }
    return initRbidSW.getStatus();
}

Status ReplicationProcess::incrementRollbackID(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto status = _storageInterface->incrementRollbackID(opCtx);

    // If the rollback ID was incremented successfully, cache the new value in _rbid to be returned
    // the next time getRollbackID() is called.
    if (status.isOK()) {
        log() << "Incremented the rollback ID to " << status.getValue();
        _rbid = status.getValue();
        invariant(kUninitializedRollbackId != _rbid);
    } else {
        warning() << "Failed to increment the rollback ID: " << status.getStatus().reason();
    }

    return status.getStatus();
}

ReplicationConsistencyMarkers* ReplicationProcess::getConsistencyMarkers() {
    return _consistencyMarkers.get();
}

ReplicationRecovery* ReplicationProcess::getReplicationRecovery() {
    return _recovery.get();
}

}  // namespace repl
}  // namespace mongo
