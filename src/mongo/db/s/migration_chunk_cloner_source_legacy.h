/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <list>
#include <set>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class BSONArrayBuilder;
class BSONObjBuilder;
class Collection;
class Database;
class PlanExecutor;
class RecordId;

class MigrationChunkClonerSourceLegacy final : public MigrationChunkClonerSource {
    MONGO_DISALLOW_COPYING(MigrationChunkClonerSourceLegacy);

public:
    MigrationChunkClonerSourceLegacy(MoveChunkRequest request, const BSONObj& shardKeyPattern);
    ~MigrationChunkClonerSourceLegacy();

    Status startClone(OperationContext* txn) override;

    Status awaitUntilCriticalSectionIsAppropriate(OperationContext* txn,
                                                  Milliseconds maxTimeToWait) override;

    Status commitClone(OperationContext* txn) override;

    void cancelClone(OperationContext* txn) override;

    bool isDocumentInMigratingChunk(OperationContext* txn, const BSONObj& doc) override;

    void onInsertOp(OperationContext* txn, const BSONObj& insertedDoc) override;

    void onUpdateOp(OperationContext* txn, const BSONObj& updatedDoc) override;

    void onDeleteOp(OperationContext* txn, const BSONObj& deletedDocId) override;

    // Legacy cloner specific functionality

    /**
     * Returns the migration session id associated with this cloner, so stale sessions can be
     * disambiguated.
     */
    const MigrationSessionId& getSessionId() const {
        return _sessionId;
    }

    /**
     * Called by the recipient shard. Used to estimate how many more bytes of clone data are
     * remaining in the chunk cloner.
     */
    uint64_t getCloneBatchBufferAllocationSize();

    /**
     * Called by the recipient shard. Populates the passed BSONArrayBuilder with a set of documents,
     * which are part of the initial clone sequence.
     *
     * Returns OK status on success. If there were documents returned in the result argument, this
     * method should be called more times until the result is empty. If it returns failure, it is
     * not safe to call more methods on this class other than cancelClone.
     *
     * This method will return early if too much time is spent fetching the documents in order to
     * give a chance to the caller to perform some form of yielding. It does not free or acquire any
     * locks on its own.
     *
     * NOTE: Must be called with the collection lock held in at least IS mode.
     */
    Status nextCloneBatch(OperationContext* txn,
                          Collection* collection,
                          BSONArrayBuilder* arrBuilder);

    /**
     * Called by the recipient shard. Transfers the accummulated local mods from source to
     * destination. Must not be called before all cloned objects have been fetched through calls to
     * nextCloneBatch.
     *
     * NOTE: Must be called with the collection lock held in at least IS mode.
     */
    Status nextModsBatch(OperationContext* txn, Database* db, BSONObjBuilder* builder);

private:
    friend class DeleteNotificationStage;
    friend class LogOpForShardingHandler;

    /**
     * Idempotent method, which cleans up any previously initialized state. It is safe to be called
     * at any time, but no methods should be called after it.
     */
    void _cleanup(OperationContext* txn);

    /**
     * Synchronously invokes the recipient shard with the specified command and either returns the
     * command response (if succeeded) or the status, if the command failed.
     */
    StatusWith<BSONObj> _callRecipient(const BSONObj& cmdObj);

    /**
     * Get the disklocs that belong to the chunk migrated and sort them in _cloneLocs (to avoid
     * seeking disk later).
     *
     * Returns OK or any error status otherwise.
     */
    Status _storeCurrentLocs(OperationContext* txn);

    /**
     * Insert items from docIdList to a new array with the given fieldName in the given builder. If
     * explode is true, the inserted object will be the full version of the document. Note that
     * whenever an item from the docList is inserted to the array, it will also be removed from
     * docList.
     *
     * Should be holding the collection lock for ns if explode is true.
     */
    void _xfer(OperationContext* txn,
               Database* db,
               std::list<BSONObj>* docIdList,
               BSONObjBuilder* builder,
               const char* fieldName,
               long long* sizeAccumulator,
               bool explode);

    // The original move chunk request
    const MoveChunkRequest _args;

    // The shard key associated with the namespace
    const ShardKeyPattern _shardKeyPattern;

    // The migration session id
    const MigrationSessionId _sessionId;

    // The resolved connection string of the donor shard
    ConnectionString _donorCS;

    // The resolved primary of the recipient shard
    HostAndPort _recipientHost;

    // Registered deletion notifications plan executor, which will listen for document deletions
    // during the cloning stage
    std::unique_ptr<PlanExecutor> _deleteNotifyExec;

    // Protects the entries below
    stdx::mutex _mutex;

    // Inidicates whether commit or cancel have already been called and ensures that we do not
    // double commit or double cancel
    bool _cloneCompleted{false};

    // List of record ids that needs to be transferred (initial clone)
    std::set<RecordId> _cloneLocs;

    // The estimated average object size during the clone phase. Used for buffer size
    // pre-allocation.
    uint64_t _averageObjectSizeForCloneLocs{0};

    // List of _id of documents that were modified that must be re-cloned.
    std::list<BSONObj> _reload;

    // List of _id of documents that were deleted during clone that should be deleted later.
    std::list<BSONObj> _deleted;

    // Total bytes in _reload + _deleted
    uint64_t _memoryUsed{0};
};

}  // namespace mongo
