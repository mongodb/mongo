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

#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "mongo/db/op_observer/op_observer.h"

namespace mongo {

/**
 * Implementation of the OpObserver interface that allows multiple observers to be registered.
 * All observers will be called in order of registration. Once an observer throws an exception,
 * no further observers will receive notifications: typically the enclosing transaction will be
 * aborted. If an observer needs to undo changes in such a case, it should register an onRollback
 * handler with the recovery unit.
 */
class OpObserverRegistry final : public OpObserver {
    OpObserverRegistry(const OpObserverRegistry&) = delete;
    OpObserverRegistry& operator=(const OpObserverRegistry&) = delete;

public:
    OpObserverRegistry() = default;
    virtual ~OpObserverRegistry() = default;

    // Add 'observer' to the list of observers to call. Observers are called in registration order.
    // Registration must be done while no calls to observers are made.
    void addObserver(std::unique_ptr<OpObserver> observer) {
        _observers.push_back(std::move(observer));
    }

    void onCreateIndex(OperationContext* const opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCreateIndex(opCtx, nss, uuid, indexDoc, fromMigrate);
    }

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onStartIndexBuild(opCtx, nss, collUUID, indexBuildUUID, indexes, fromMigrate);
        }
    }

    void onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                      const NamespaceString& nss) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onStartIndexBuildSinglePhase(opCtx, nss);
        }
    }

    void onAbortIndexBuildSinglePhase(OperationContext* opCtx,
                                      const NamespaceString& nss) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onAbortIndexBuildSinglePhase(opCtx, nss);
        }
    }

    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const UUID& collUUID,
                            const UUID& indexBuildUUID,
                            const std::vector<BSONObj>& indexes,
                            bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onCommitIndexBuild(opCtx, nss, collUUID, indexBuildUUID, indexes, fromMigrate);
        }
    }

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           const Status& cause,
                           bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onAbortIndexBuild(opCtx, nss, collUUID, indexBuildUUID, indexes, cause, fromMigrate);
        }
    }

    void onInserts(OperationContext* const opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onInserts(opCtx, coll, begin, end, fromMigrate);
    }

    void onUpdate(OperationContext* const opCtx, const OplogUpdateEntryArgs& args) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onUpdate(opCtx, args);
    }

    void aboutToDelete(OperationContext* const opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const BSONObj& doc) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->aboutToDelete(opCtx, nss, uuid, doc);
    }

    void onDelete(OperationContext* const opCtx,
                  const NamespaceString& nss,
                  const UUID& uuid,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDelete(opCtx, nss, uuid, stmtId, args);
    }

    void onInternalOpMessage(OperationContext* const opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj,
                             const boost::optional<repl::OpTime> preImageOpTime,
                             const boost::optional<repl::OpTime> postImageOpTime,
                             const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                             const boost::optional<OplogSlot> slot) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onInternalOpMessage(opCtx,
                                   nss,
                                   uuid,
                                   msgObj,
                                   o2MsgObj,
                                   preImageOpTime,
                                   postImageOpTime,
                                   prevWriteOpTimeInTransaction,
                                   slot);
    }

    void onCreateCollection(OperationContext* const opCtx,
                            const CollectionPtr& coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime,
                            bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCreateCollection(
                opCtx, coll, collectionName, options, idIndex, createOpTime, fromMigrate);
    }

    void onCollMod(OperationContext* const opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCollMod(opCtx, nss, uuid, collModCmd, oldCollOptions, indexInfo);
    }

    void onDropDatabase(OperationContext* const opCtx, const DatabaseName& dbName) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDropDatabase(opCtx, dbName);
    }

    repl::OpTime onDropCollection(OperationContext* const opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  const CollectionDropType dropType) override {
        return onDropCollection(
            opCtx, collectionName, uuid, numRecords, dropType, false /* markFromMigrate*/);
    }

    repl::OpTime onDropCollection(OperationContext* const opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  const CollectionDropType dropType,
                                  bool markFromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& observer : this->_observers) {
            auto time = observer->onDropCollection(
                opCtx, collectionName, uuid, numRecords, dropType, markFromMigrate);
            invariant(time.isNull());
        }
        return _getOpTimeToReturn(times.get().reservedOpTimes);
    }

    void onDropIndex(OperationContext* const opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDropIndex(opCtx, nss, uuid, indexName, idxDescriptor);
    }

    void onRenameCollection(OperationContext* const opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp) override {
        onRenameCollection(opCtx,
                           fromCollection,
                           toCollection,
                           uuid,
                           dropTargetUUID,
                           numRecords,
                           stayTemp,
                           false /* markFromMigrate */);
    }

    void onRenameCollection(OperationContext* const opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp,
                            bool markFromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onRenameCollection(opCtx,
                                  fromCollection,
                                  toCollection,
                                  uuid,
                                  dropTargetUUID,
                                  numRecords,
                                  stayTemp,
                                  markFromMigrate);
    }

    void onImportCollection(OperationContext* opCtx,
                            const UUID& importUUID,
                            const NamespaceString& nss,
                            long long numRecords,
                            long long dataSize,
                            const BSONObj& catalogEntry,
                            const BSONObj& storageMetadata,
                            bool isDryRun) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onImportCollection(opCtx,
                                  importUUID,
                                  nss,
                                  numRecords,
                                  dataSize,
                                  catalogEntry,
                                  storageMetadata,
                                  isDryRun);
    }

    repl::OpTime preRenameCollection(OperationContext* const opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp) override {
        return preRenameCollection(opCtx,
                                   fromCollection,
                                   toCollection,
                                   uuid,
                                   dropTargetUUID,
                                   numRecords,
                                   stayTemp,
                                   false /* markFromMigrate */);
    }

    repl::OpTime preRenameCollection(OperationContext* const opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp,
                                     bool markFromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& observer : this->_observers) {
            const auto time = observer->preRenameCollection(opCtx,
                                                            fromCollection,
                                                            toCollection,
                                                            uuid,
                                                            dropTargetUUID,
                                                            numRecords,
                                                            stayTemp,
                                                            markFromMigrate);
            invariant(time.isNull());
        }
        return _getOpTimeToReturn(times.get().reservedOpTimes);
    }

    void postRenameCollection(OperationContext* const opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              const UUID& uuid,
                              const boost::optional<UUID>& dropTargetUUID,
                              bool stayTemp) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->postRenameCollection(
                opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    }
    void onApplyOps(OperationContext* const opCtx,
                    const DatabaseName& dbName,
                    const BSONObj& applyOpCmd) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onApplyOps(opCtx, dbName, applyOpCmd);
    }

    void onEmptyCapped(OperationContext* const opCtx,
                       const NamespaceString& collectionName,
                       const UUID& uuid) {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onEmptyCapped(opCtx, collectionName, uuid);
    }

    void onUnpreparedTransactionCommit(OperationContext* opCtx,
                                       std::vector<repl::ReplOperation>* statements,
                                       size_t numberOfPrePostImagesToWrite) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onUnpreparedTransactionCommit(opCtx, statements, numberOfPrePostImagesToWrite);
    }

    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onPreparedTransactionCommit(
                opCtx, commitOplogEntryOpTime, commitTimestamp, statements);
    }

    std::unique_ptr<ApplyOpsOplogSlotAndOperationAssignment> preTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        std::vector<repl::ReplOperation>* statements) override {
        std::unique_ptr<ApplyOpsOplogSlotAndOperationAssignment>
            applyOpsOplogSlotAndOperationAssignment;
        for (auto&& observer : _observers) {
            auto applyOpsAssignment = observer->preTransactionPrepare(
                opCtx, reservedSlots, numberOfPrePostImagesToWrite, wallClockTime, statements);
            tassert(6278501,
                    "More than one OpObserver returned operation to \"applyOps\" assignment",
                    !(applyOpsAssignment && applyOpsOplogSlotAndOperationAssignment));
            if (applyOpsAssignment) {
                applyOpsOplogSlotAndOperationAssignment = std::move(applyOpsAssignment);
            }
        }
        return applyOpsOplogSlotAndOperationAssignment;
    }

    void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        std::vector<repl::ReplOperation>* statements,
        const ApplyOpsOplogSlotAndOperationAssignment* applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime) override {
        ReservedTimes times{opCtx};
        for (auto& observer : _observers) {
            observer->onTransactionPrepare(opCtx,
                                           reservedSlots,
                                           statements,
                                           applyOpsOperationAssignment,
                                           numberOfPrePostImagesToWrite,
                                           wallClockTime);
        }
    }

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onTransactionAbort(opCtx, abortOplogEntryOpTime);
    }

    void onBatchedWriteStart(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onBatchedWriteStart(opCtx);
        }
    }

    void onBatchedWriteCommit(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onBatchedWriteCommit(opCtx);
        }
    }

    void onBatchedWriteAbort(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onBatchedWriteAbort(opCtx);
        }
    }

    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) override {
        for (auto& o : _observers)
            o->onMajorityCommitPointUpdate(service, newCommitPoint);
    }

private:
    void _onReplicationRollback(OperationContext* opCtx,
                                const RollbackObserverInfo& rbInfo) override {
        for (auto& o : _observers)
            o->onReplicationRollback(opCtx, rbInfo);
    }

    static repl::OpTime _getOpTimeToReturn(const std::vector<repl::OpTime>& times) {
        if (times.empty()) {
            return repl::OpTime{};
        }
        invariant(times.size() == 1);
        return times.front();
    }

    std::vector<std::unique_ptr<OpObserver>> _observers;
};
}  // namespace mongo
