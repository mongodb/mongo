/**
 *    Copyright 2016 MongoDB Inc.
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

#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "mongo/db/op_observer.h"

namespace mongo {

/**
 * Implementation of the OpObserver interface that allows multiple observers to be registered.
 * All observers will be called in order of registration. Once an observer throws an exception,
 * no further observers will receive notifications: typically the enclosing transaction will be
 * aborted. If an observer needs to undo changes in such a case, it should register an onRollback
 * handler with the recovery unit.
 */
class OpObserverRegistry final : public OpObserver {
    MONGO_DISALLOW_COPYING(OpObserverRegistry);

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
                       OptionalCollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCreateIndex(opCtx, nss, uuid, indexDoc, fromMigrate);
    }

    void onInserts(OperationContext* const opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onInserts(opCtx, nss, uuid, begin, end, fromMigrate);
    }

    void onUpdate(OperationContext* const opCtx, const OplogUpdateEntryArgs& args) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onUpdate(opCtx, args);
    }

    void aboutToDelete(OperationContext* const opCtx,
                       const NamespaceString& nss,
                       const BSONObj& doc) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->aboutToDelete(opCtx, nss, doc);
    }

    void onDelete(OperationContext* const opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDelete(opCtx, nss, uuid, stmtId, fromMigrate, deletedDoc);
    }

    void onInternalOpMessage(OperationContext* const opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onInternalOpMessage(opCtx, nss, uuid, msgObj, o2MsgObj);
    }

    void onCreateCollection(OperationContext* const opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCreateCollection(opCtx, coll, collectionName, options, idIndex);
    }

    void onCollMod(OperationContext* const opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<TTLCollModInfo> ttlInfo) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onCollMod(opCtx, nss, uuid, collModCmd, oldCollOptions, ttlInfo);
    }

    void onDropDatabase(OperationContext* const opCtx, const std::string& dbName) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDropDatabase(opCtx, dbName);
    }

    repl::OpTime onDropCollection(OperationContext* const opCtx,
                                  const NamespaceString& collectionName,
                                  const OptionalCollectionUUID uuid) override {
        ReservedTimes times{opCtx};
        for (auto& observer : this->_observers) {
            auto time = observer->onDropCollection(opCtx, collectionName, uuid);
            invariant(time.isNull());
        }
        return _getOpTimeToReturn(times.get().reservedOpTimes);
    }

    void onDropIndex(OperationContext* const opCtx,
                     const NamespaceString& nss,
                     OptionalCollectionUUID uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onDropIndex(opCtx, nss, uuid, indexName, idxDescriptor);
    }


    void onRenameCollection(OperationContext* const opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            bool stayTemp) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onRenameCollection(
                opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    }

    repl::OpTime preRenameCollection(OperationContext* const opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     bool stayTemp) override {
        ReservedTimes times{opCtx};
        for (auto& observer : this->_observers) {
            const auto time = observer->preRenameCollection(
                opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
            invariant(time.isNull());
        }
        return _getOpTimeToReturn(times.get().reservedOpTimes);
    }

    void postRenameCollection(OperationContext* const opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->postRenameCollection(
                opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
    }
    void onApplyOps(OperationContext* const opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onApplyOps(opCtx, dbName, applyOpCmd);
    }

    void onEmptyCapped(OperationContext* const opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid) {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onEmptyCapped(opCtx, collectionName, uuid);
    }

    void onTransactionCommit(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onTransactionCommit(opCtx);
    }

    void onTransactionPrepare(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& observer : _observers) {
            observer->onTransactionPrepare(opCtx);
        }
    }

    void onTransactionAbort(OperationContext* opCtx) override {
        ReservedTimes times{opCtx};
        for (auto& o : _observers)
            o->onTransactionAbort(opCtx);
    }

    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) override {
        for (auto& o : _observers)
            o->onReplicationRollback(opCtx, rbInfo);
    }

private:
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
