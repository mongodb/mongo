/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo {

/**
 * OpObserver which is installed on the op observers chain when the server is running as a shard
 * server (--shardsvr).
 */
class ShardServerOpObserver final : public OpObserver {
    MONGO_DISALLOW_COPYING(ShardServerOpObserver);

public:
    ShardServerOpObserver();
    ~ShardServerOpObserver();

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       OptionalCollectionUUID uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) override {}

    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override;

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override;

    void aboutToDelete(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const BSONObj& doc) override;

    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) override;

    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj) override {}

    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex) override {}

    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<TTLCollModInfo> ttlInfo) override {}

    void onDropDatabase(OperationContext* opCtx, const std::string& dbName) override {}

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid) override;

    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     OptionalCollectionUUID uuid,
                     const std::string& indexName,
                     const BSONObj& indexInfo) override {}

    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            OptionalCollectionUUID uuid,
                            OptionalCollectionUUID dropTargetUUID,
                            bool stayTemp) override {}
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     OptionalCollectionUUID uuid,
                                     OptionalCollectionUUID dropTargetUUID,
                                     bool stayTemp) override {
        return repl::OpTime();
    }
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              OptionalCollectionUUID uuid,
                              OptionalCollectionUUID dropTargetUUID,
                              bool stayTemp) override {}

    void onApplyOps(OperationContext* opCtx,
                    const std::string& dbName,
                    const BSONObj& applyOpCmd) override {}

    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       OptionalCollectionUUID uuid) override {}

    void onTransactionCommit(OperationContext* opCtx) override {}

    void onTransactionPrepare(OperationContext* opCtx) override {}

    void onTransactionAbort(OperationContext* opCtx) override {}

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) {}
};


// Replication oplog OpObserver hooks. Informs the sharding system of changes that may be
// relevant to ongoing operations.
//
// The global lock is expected to be held in mode IX by the caller of any of these functions.

/**
 * Details of documents being removed from a sharded collection.
 */
struct ShardObserverDeleteState {
    static ShardObserverDeleteState make(OperationContext* opCtx,
                                         CollectionShardingState* css,
                                         const BSONObj& docToDelete);
    // Contains the fields of the document that are in the collection's shard key, and "_id".
    BSONObj documentKey;

    // True if the document being deleted belongs to a chunk which, while still in the shard,
    // is being migrated out. (Not to be confused with "fromMigrate", which tags operations
    // that are steps in performing the migration.)
    bool isMigrating;
};

void shardObserveInsertOp(OperationContext* opCtx,
                          CollectionShardingState* css,
                          const BSONObj& insertedDoc,
                          const repl::OpTime& opTime);
void shardObserveUpdateOp(OperationContext* opCtx,
                          CollectionShardingState* css,
                          const BSONObj& updatedDoc,
                          const repl::OpTime& opTime,
                          const repl::OpTime& prePostImageOpTime);
void shardObserveDeleteOp(OperationContext* opCtx,
                          CollectionShardingState* css,
                          const ShardObserverDeleteState& deleteState,
                          const repl::OpTime& opTime,
                          const repl::OpTime& preImageOpTime);

}  // namespace mongo
