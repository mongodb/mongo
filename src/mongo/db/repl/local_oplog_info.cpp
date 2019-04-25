/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/local_oplog_info.h"

#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {

const auto localOplogInfo = ServiceContext::declareDecoration<LocalOplogInfo>();

}  // namespace

// static
LocalOplogInfo* LocalOplogInfo::get(ServiceContext& service) {
    return get(&service);
}

// static
LocalOplogInfo* LocalOplogInfo::get(ServiceContext* service) {
    return &localOplogInfo(service);
}

// static
LocalOplogInfo* LocalOplogInfo::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

const NamespaceString& LocalOplogInfo::getOplogCollectionName() const {
    return _oplogName;
}

void LocalOplogInfo::setOplogCollectionName(ServiceContext* service) {
    switch (ReplicationCoordinator::get(service)->getReplicationMode()) {
        case ReplicationCoordinator::modeReplSet:
            _oplogName = NamespaceString::kRsOplogNamespace;
            break;
        case ReplicationCoordinator::modeNone:
            // leave empty.
            break;
    }
}

Collection* LocalOplogInfo::getCollection() const {
    return _oplog;
}

void LocalOplogInfo::setCollection(Collection* oplog) {
    _oplog = oplog;
}

void LocalOplogInfo::resetCollection() {
    _oplog = nullptr;
}

void LocalOplogInfo::setNewTimestamp(ServiceContext* service, const Timestamp& newTime) {
    stdx::lock_guard<stdx::mutex> lk(_newOpMutex);
    LogicalClock::get(service)->setClusterTimeFromTrustedSource(LogicalTime(newTime));
}

std::vector<OplogSlot> LocalOplogInfo::getNextOpTimes(OperationContext* opCtx, std::size_t count) {
    auto replCoord = ReplicationCoordinator::get(opCtx);
    long long term = OpTime::kUninitializedTerm;

    // Fetch term out of the newOpMutex.
    if (replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet) {
        // Current term. If we're not a replset of pv=1, it remains kOldProtocolVersionTerm.
        term = replCoord->getTerm();
    }

    Timestamp ts;
    // Provide a sample to FlowControl after the `oplogInfo.newOpMutex` is released.
    ON_BLOCK_EXIT([opCtx, &ts, count] {
        auto flowControl = FlowControl::get(opCtx);
        if (flowControl) {
            flowControl->sample(ts, count);
        }
    });

    // Allow the storage engine to start the transaction outside the critical section.
    opCtx->recoveryUnit()->preallocateSnapshot();
    stdx::lock_guard<stdx::mutex> lk(_newOpMutex);

    ts = LogicalClock::get(opCtx)->reserveTicks(count).asTimestamp();
    const bool orderedCommit = false;

    // The local oplog collection pointer must already be established by this point.
    // We can't establish it here because that would require locking the local database, which would
    // be a lock order violation.
    invariant(_oplog);
    fassert(28560, _oplog->getRecordStore()->oplogDiskLocRegister(opCtx, ts, orderedCommit));

    std::vector<OplogSlot> oplogSlots(count);
    for (std::size_t i = 0; i < count; i++) {
        oplogSlots[i] = {Timestamp(ts.asULL() + i), term};
    }
    return oplogSlots;
}

}  // namespace repl
}  // namespace mongo
