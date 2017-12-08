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
        _observers.emplace_back(std::move(observer));
    }

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       OptionalCollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override {
        for (auto& o : _observers)
            o->onCreateIndex(opCtx, nss, uuid, indexDoc, fromMigrate);
    }

    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override {
        for (auto& o : _observers)
            o->onInserts(opCtx, nss, uuid, begin, end, fromMigrate);
    }

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override {
        for (auto& o : _observers)
            o->onUpdate(opCtx, args);
    }

    void aboutToDelete(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const BSONObj& doc) override {
        for (auto& o : _observers)
            o->aboutToDelete(opCtx, nss, doc);
    }

    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) override {
        for (auto& o : _observers)
            o->onDelete(opCtx, nss, uuid, stmtId, fromMigrate, deletedDoc);
    }

    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj) override {
        for (auto& o : _observers)
            o->onInternalOpMessage(opCtx, nss, uuid, msgObj, o2MsgObj);
    }

    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex) override {
        for (auto& o : _observers)
            o->onCreateCollection(opCtx, coll, collectionName, options, idIndex);
    }

    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<TTLCollModInfo> ttlInfo) override {
        for (auto& o : _observers)
            o->onCollMod(opCtx, nss, uuid, collModCmd, oldCollOptions, ttlInfo);
    }

    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) override {
        for (auto& o : _observers)
            o->onDropDatabase(opCtx, dbName);
    }
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) override {
        return _forEachObserver([&](auto& observer) -> repl::OpTime {
            return observer.onDropCollection(opCtx, collectionName, uuid);
        });
    }

    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     OptionalCollectionUUID uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor) override {
        for (auto& o : _observers)
            o->onDropIndex(opCtx, nss, uuid, indexName, idxDescriptor);
    }

    repl::OpTime onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    OptionalCollectionUUID uuid,
                                    bool dropTarget,
                                    OptionalCollectionUUID dropTargetUUID,
                                    bool stayTemp) override {
        return _forEachObserver([&](auto& observer) -> repl::OpTime {
            return observer.onRenameCollection(
                opCtx, fromCollection, toCollection, uuid, dropTarget, dropTargetUUID, stayTemp);
        });
    }

    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override {
        for (auto& o : _observers)
            o->onApplyOps(opCtx, dbName, applyOpCmd);
    }

    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid) {
        for (auto& o : _observers)
            o->onEmptyCapped(opCtx, collectionName, uuid);
    }

private:
    repl::OpTime _forEachObserver(stdx::function<repl::OpTime(OpObserver&)> f) {
        repl::OpTime opTime;
        for (auto& observer : _observers) {
            repl::OpTime newTime = f(*observer);
            if (!newTime.isNull() && newTime != opTime) {
                invariant(opTime.isNull());
                opTime = newTime;
            }
        }
        return opTime;
    }
    std::vector<std::unique_ptr<OpObserver>> _observers;
};
}  // namespace mongo
