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
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

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
    OpObserverRegistry();
    virtual ~OpObserverRegistry();

    // This implementaton is unused, but needs to be implemented to conform to the OpObserver
    // interface.
    NamespaceFilters getNamespaceFilters() const {
        return {NamespaceFilter::kAll, NamespaceFilter::kAll};
    }

    // Add 'observer' to the list of observers to call. Observers are called in registration order.
    // Registration must be done while no calls to observers are made.
    void addObserver(std::unique_ptr<OpObserver> observer) {
        const auto& nsFilters = observer->getNamespaceFilters();
        _observers.push_back(std::move(observer));

        OpObserver* observerPtr = _observers.back().get();
        switch (nsFilters.updateFilter) {
            case OpObserver::NamespaceFilter::kConfig:
                _insertAndUpdateConfigObservers.push_back(observerPtr);
                break;
            case OpObserver::NamespaceFilter::kSystem:
                _insertAndUpdateSystemObservers.push_back(observerPtr);
                break;
            case OpObserver::NamespaceFilter::kConfigAndSystem:
                _insertAndUpdateConfigObservers.push_back(observerPtr);
                _insertAndUpdateSystemObservers.push_back(observerPtr);
                break;
            case OpObserver::NamespaceFilter::kAll:
                _insertAndUpdateConfigObservers.push_back(observerPtr);
                _insertAndUpdateSystemObservers.push_back(observerPtr);
                _insertAndUpdateUserObservers.push_back(observerPtr);
                break;
            default:
                break;
        }

        switch (nsFilters.deleteFilter) {
            case OpObserver::NamespaceFilter::kConfig:
                _onDeleteConfigObservers.push_back(observerPtr);
                break;
            case OpObserver::NamespaceFilter::kSystem:
                _onDeleteSystemObservers.push_back(observerPtr);
                break;
            case OpObserver::NamespaceFilter::kConfigAndSystem:
                _onDeleteConfigObservers.push_back(observerPtr);
                _onDeleteSystemObservers.push_back(observerPtr);
                break;
            case OpObserver::NamespaceFilter::kAll:
                _onDeleteConfigObservers.push_back(observerPtr);
                _onDeleteSystemObservers.push_back(observerPtr);
                _onDeleteUserObservers.push_back(observerPtr);
                break;
            default:
                break;
        }
    }

    void onModifyCollectionShardingIndexCatalog(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const UUID& uuid,
                                                BSONObj indexDoc) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onModifyCollectionShardingIndexCatalog(opCtx, nss, uuid, indexDoc);
    }

    void onCreateGlobalIndex(OperationContext* opCtx,
                             const NamespaceString& globalIndexNss,
                             const UUID& globalIndexUUID) final {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCreateGlobalIndex(opCtx, globalIndexNss, globalIndexUUID);
    };

    void onDropGlobalIndex(OperationContext* opCtx,
                           const NamespaceString& globalIndexNss,
                           const UUID& globalIndexUUID,
                           long long numKeys) final {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDropGlobalIndex(opCtx, globalIndexNss, globalIndexUUID, numKeys);
    };

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
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;

        const auto& nss = coll->ns();
        std::vector<OpObserver*>* observerQueue;
        if (nss.isConfigDB()) {
            observerQueue = &_insertAndUpdateConfigObservers;
        } else if (nss.isSystem()) {
            observerQueue = &_insertAndUpdateSystemObservers;
        } else {
            observerQueue = &_insertAndUpdateUserObservers;
        }

        for (auto& o : *observerQueue)
            o->onInserts(opCtx,
                         coll,
                         begin,
                         end,
                         recordIds,
                         fromMigrate,
                         defaultFromMigrate,
                         &opStateAccumulator);
    }

    void onInsertGlobalIndexKey(OperationContext* opCtx,
                                const NamespaceString& globalIndexNss,
                                const UUID& globalIndexUuid,
                                const BSONObj& key,
                                const BSONObj& docKey) override {

        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onInsertGlobalIndexKey(opCtx, globalIndexNss, globalIndexUuid, key, docKey);
    }

    void onDeleteGlobalIndexKey(OperationContext* opCtx,
                                const NamespaceString& globalIndexNss,
                                const UUID& globalIndexUuid,
                                const BSONObj& key,
                                const BSONObj& docKey) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDeleteGlobalIndexKey(opCtx, globalIndexNss, globalIndexUuid, key, docKey);
    }

    void onUpdate(OperationContext* const opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;

        const auto& nss = args.coll->ns();
        std::vector<OpObserver*>* observerQueue;
        if (nss.isConfigDB()) {
            observerQueue = &_insertAndUpdateConfigObservers;
        } else if (nss.isSystem()) {
            observerQueue = &_insertAndUpdateSystemObservers;
        } else {
            observerQueue = &_insertAndUpdateUserObservers;
        }

        for (auto& o : *observerQueue)
            o->onUpdate(opCtx, args, &opStateAccumulator);
    }

    void aboutToDelete(OperationContext* const opCtx,
                       const CollectionPtr& coll,
                       const BSONObj& doc,
                       OplogDeleteEntryArgs* args,
                       OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;

        const auto& nss = coll->ns();
        std::vector<OpObserver*>* observerQueue;
        if (nss.isConfigDB()) {
            observerQueue = &_onDeleteConfigObservers;
        } else if (nss.isSystem()) {
            observerQueue = &_onDeleteSystemObservers;
        } else {
            observerQueue = &_onDeleteUserObservers;
        }

        for (auto& o : *observerQueue)
            o->aboutToDelete(opCtx, coll, doc, args, &opStateAccumulator);
    }

    void onDelete(OperationContext* const opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;

        const auto& nss = coll->ns();
        std::vector<OpObserver*>* observerQueue;
        if (nss.isConfigDB()) {
            observerQueue = &_onDeleteConfigObservers;
        } else if (nss.isSystem()) {
            observerQueue = &_onDeleteSystemObservers;
        } else {
            observerQueue = &_onDeleteUserObservers;
        }

        for (auto& o : *observerQueue)
            o->onDelete(opCtx, coll, stmtId, doc, args, &opStateAccumulator);
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

    void onEmptyCapped(OperationContext* const opCtx,
                       const NamespaceString& collectionName,
                       const UUID& uuid) {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onEmptyCapped(opCtx, collectionName, uuid);
    }

    void onTransactionStart(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onTransactionStart(opCtx);
        }
    }

    void onUnpreparedTransactionCommit(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;
        for (auto& o : _observers)
            o->onUnpreparedTransactionCommit(opCtx,
                                             reservedSlots,
                                             transactionOperations,
                                             applyOpsOperationAssignment,
                                             &opStateAccumulator);
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

    void preTransactionPrepare(
        OperationContext* opCtx,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        Date_t wallClockTime) override {
        for (auto&& observer : _observers) {
            observer->preTransactionPrepare(
                opCtx, transactionOperations, applyOpsOperationAssignment, wallClockTime);
        }
    }

    void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;
        for (auto& observer : _observers) {
            observer->onTransactionPrepare(opCtx,
                                           reservedSlots,
                                           transactionOperations,
                                           applyOpsOperationAssignment,
                                           numberOfPrePostImagesToWrite,
                                           wallClockTime,
                                           &opStateAccumulator);
        }
    }

    void postTransactionPrepare(OperationContext* opCtx,
                                const std::vector<OplogSlot>& reservedSlots,
                                const TransactionOperations& transactionOperations) override {
        ReservedTimes times{opCtx};
        for (auto& observer : _observers) {
            observer->postTransactionPrepare(opCtx, reservedSlots, transactionOperations);
        }
    }

    void onTransactionPrepareNonPrimary(OperationContext* opCtx,
                                        const LogicalSessionId& lsid,
                                        const std::vector<repl::OplogEntry>& statements,
                                        const repl::OpTime& prepareOpTime) override {
        ReservedTimes times{opCtx};
        for (auto& observer : _observers) {
            observer->onTransactionPrepareNonPrimary(opCtx, lsid, statements, prepareOpTime);
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

    void onBatchedWriteCommit(OperationContext* opCtx,
                              WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                              OpStateAccumulator* opAccumulator = nullptr) override {
        ReservedTimes times{opCtx};
        OpStateAccumulator opStateAccumulator;
        for (auto& o : _observers) {
            o->onBatchedWriteCommit(opCtx, oplogGroupingFormat, &opStateAccumulator);
        }
    }

    void onBatchedWriteAbort(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers) {
            o->onBatchedWriteAbort(opCtx);
        }
    }

    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) override {
        for (auto& o : _observers)
            o->onReplicationRollback(opCtx, rbInfo);
    }

    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) override {
        for (auto& o : _observers)
            o->onMajorityCommitPointUpdate(service, newCommitPoint);
    }

private:
    static repl::OpTime _getOpTimeToReturn(const std::vector<repl::OpTime>& times) {
        if (times.empty()) {
            return repl::OpTime{};
        }
        invariant(times.size() == 1);
        return times.front();
    }

    // For use by the long tail of non-performance-critical operations: non-CRUD.
    // CRUD operations have the most observers and are worth optimizing. For non-CRUD operations,
    // there are few implemented observers and as little as one that implement the interface.
    std::vector<std::unique_ptr<OpObserver>> _observers;

    // For performance reasons, store separate but still ordered queues for CRUD ops.
    // Each CRUD operation will iterate through one of these queues based on the nss
    // of the target document of the operation.
    std::vector<OpObserver*> _insertAndUpdateConfigObservers;  // config.*
    std::vector<OpObserver*> _insertAndUpdateSystemObservers;  // *.system.*
    std::vector<OpObserver*>
        _insertAndUpdateUserObservers;  // not config nor system.
                                        // Will impact writes to all user collections.

    // Having separate queues for delete operations allows observers like
    // PrimaryOnlyServiceOpObserver to use the filtering differently between insert/update, and
    // delete.
    std::vector<OpObserver*> _onDeleteConfigObservers;  // config.*
    std::vector<OpObserver*> _onDeleteSystemObservers;  // *.system.*
    std::vector<OpObserver*> _onDeleteUserObservers;    // not config nor system
                                                      // Will impact writes to all user collections.
};
}  // namespace mongo
