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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTlaPlusTrace

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/tla_plus_trace_repl_gen.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/tla_plus_trace.h"

namespace mongo::repl {

void ReplicationCoordinatorImpl::tlaPlusRaftMongoEvent(
    OperationContext* opCtx,
    RaftMongoSpecActionEnum action,
    boost::optional<Timestamp> oplogReadTimestamp) const {
    if (MONGO_unlikely(logForTLAPlusSpecs.shouldFail())) {
        stdx::unique_lock<Latch> lk(_mutex);
        _tlaPlusRaftMongoEvent(lk, opCtx, action, oplogReadTimestamp);
    }
}

void ReplicationCoordinatorImpl::_tlaPlusRaftMongoEvent(
    WithLock,
    OperationContext* opCtx,
    RaftMongoSpecActionEnum action,
    boost::optional<Timestamp> oplogReadTimestamp) const noexcept {
    if (MONGO_unlikely(
            logForTLAPlusSpecs.scopedIf(enabledForSpec(TLAPlusSpecEnum::kRaftMongo)).isActive())) {
        auto actionName = RaftMongoSpecAction_serializer(action);

        ShouldNotConflictWithSecondaryBatchApplicationBlock noConflict(opCtx->lockState());

        // Read the oplog collection without using DBLock or AutoGetCollectionForRead, they take
        // RSTL while we hold _mutex, which is the wrong lock order and risks deadlock. We don't
        // need the RSTL because _mutex protects us from state changes. Global IS lock should
        // suffice to read the oplog, but lock the DB and collection so we can read from the oplog
        // and prevent a rollback from truncating it while we read.
        // TODO (SERVER-44906): Remove db and collection locks.
        LOG(2) << "Getting global lock in IS mode to log " << actionName << " for RaftMongo.tla";

        opCtx->lockState()->lockGlobal(opCtx, MODE_IS);
        ON_BLOCK_EXIT([&opCtx]() { opCtx->lockState()->unlockGlobal(); });

        const auto oplogNs = NamespaceString::kRsOplogNamespace;
        const auto dbResource = ResourceId(RESOURCE_DATABASE, oplogNs.db());
        opCtx->lockState()->lock(opCtx, dbResource, MODE_IS);
        ON_BLOCK_EXIT([&opCtx, &dbResource]() { opCtx->lockState()->unlock(dbResource); });

        const auto collResource = ResourceId(RESOURCE_COLLECTION, oplogNs.ns());
        opCtx->lockState()->lock(opCtx, collResource, MODE_IS);
        ON_BLOCK_EXIT([&opCtx, &collResource]() { opCtx->lockState()->unlock(collResource); });

        auto oplogCollection =
            CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, oplogNs);
        invariant(oplogCollection);

        // If the recovery unit is inactive, ensure it's still inactive after this function.
        std::unique_ptr<RecoveryUnit> oldRecoveryUnit;
        auto oldRecoveryUnitState = WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork;
        if (opCtx->recoveryUnit()->getState() == RecoveryUnit::State::kInactive) {
            oldRecoveryUnit = opCtx->releaseRecoveryUnit();
            oldRecoveryUnitState = opCtx->setRecoveryUnit(
                std::unique_ptr<RecoveryUnit>(
                    opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        }

        ON_BLOCK_EXIT([&opCtx, &oldRecoveryUnit, &oldRecoveryUnitState]() {
            if (oldRecoveryUnit) {
                opCtx->setRecoveryUnit(std::move(oldRecoveryUnit), oldRecoveryUnitState);
            };
        });

        auto oplogRecordStore = oplogCollection->getRecordStore();
        auto isPrimary = _getMemberState_inlock() == MemberState::RS_PRIMARY;
        auto serverState = isPrimary ? RaftMongoSpecServerStateEnum::kLeader
                                     : RaftMongoSpecServerStateEnum::kFollower;

        LOG(2) << "Going to log " << actionName << " as "
               << RaftMongoSpecServerState_serializer(serverState)
               << " for RaftMongo.tla, reading oplog with timestamp " << oplogReadTimestamp;

        // On a primary, read the oplog forward with default oplog visibility rules. Same on a
        // secondary, but if a timestamp is provided, read backwards to see all entries.
        auto backward = oplogReadTimestamp.has_value();
        auto cursor = oplogRecordStore->getCursor(opCtx, !backward);
        std::vector<OpTime> entryOpTimes;
        while (auto record = cursor->next()) {
            auto opTime = OplogEntry(record.get().data.toBson()).getOpTime();
            if (!oplogReadTimestamp || oplogReadTimestamp >= opTime.getTimestamp()) {
                entryOpTimes.emplace_back(opTime);
            }
        }

        TlaPlusTraceEvent event;
        event.setSpec(TLAPlusSpecEnum::kRaftMongo);
        event.setAction(RaftMongoSpecAction_serializer(action));

        RaftMongoSpecEvent raftEvent;
        raftEvent.setServerState(serverState);
        raftEvent.setCommitPoint(_topCoord->getLastCommittedOpTime());
        raftEvent.setTerm(getTerm());

        if (backward) {
            std::reverse(entryOpTimes.begin(), entryOpTimes.end());
        }
        raftEvent.setLog(entryOpTimes);
        event.setState(raftEvent.toBSON());
        logTlaPlusTraceEvent(event);
    }
}
}  // namespace mongo::repl
