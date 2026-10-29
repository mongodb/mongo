// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/replication_process.h"

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


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
    std::lock_guard<std::mutex> lock(_mutex);

    auto rbidResult = _storageInterface->getRollbackID(opCtx);
    if (!rbidResult.isOK()) {
        return rbidResult.getStatus();
    }

    if (kUninitializedRollbackId == _rbid) {
        LOGV2(21529, "Initializing rollback ID", "rbid"_attr = rbidResult.getValue());
    } else {
        LOGV2(21530,
              "Setting rollback ID",
              "rbid"_attr = rbidResult.getValue(),
              "previousRBID"_attr = _rbid);
    }
    _rbid = rbidResult.getValue();

    return Status::OK();
}

int ReplicationProcess::getRollbackID() const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (kUninitializedRollbackId == _rbid) {
        // This may happen when serverStatus is called by an internal client before we have a chance
        // to read the rollback ID from storage.
        LOGV2_DEBUG(21533, 3, "Rollback ID is not initialized yet");
    }
    return _rbid;
}

Status ReplicationProcess::initializeRollbackID(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_mutex);

    invariant(kUninitializedRollbackId == _rbid);

    // Do not make any assumptions about the starting value of the rollback ID in the
    // local.system.rollback.id collection other than it cannot be "kUninitializedRollbackId".
    // Cache the rollback ID in _rbid to be returned the next time getRollbackID() is called.

    auto initRbidSW = _storageInterface->initializeRollbackID(opCtx);
    if (initRbidSW.isOK()) {
        LOGV2(21531, "Initialized the rollback ID", "rbid"_attr = initRbidSW.getValue());
        _rbid = initRbidSW.getValue();
        invariant(kUninitializedRollbackId != _rbid);
    } else {
        LOGV2_WARNING(21534,
                      "Failed to initialize the rollback ID",
                      "error"_attr = initRbidSW.getStatus().reason());
    }
    return initRbidSW.getStatus();
}

Status ReplicationProcess::incrementRollbackID(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto status = _storageInterface->incrementRollbackID(opCtx);

    // If the rollback ID was incremented successfully, cache the new value in _rbid to be returned
    // the next time getRollbackID() is called.
    if (status.isOK()) {
        LOGV2(21532, "Incremented the rollback ID", "rbid"_attr = status.getValue());
        _rbid = status.getValue();
        invariant(kUninitializedRollbackId != _rbid);
    } else {
        LOGV2_WARNING(21535,
                      "Failed to increment the rollback ID",
                      "error"_attr = status.getStatus().reason());
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
