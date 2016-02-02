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
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

class Database;
class OperationContext;
class PlanExecutor;
class RecordId;

class MigrationSourceManager {
    MONGO_DISALLOW_COPYING(MigrationSourceManager);

public:
    MigrationSourceManager();
    ~MigrationSourceManager();

    /**
     * Returns false if cannot start. One of the reason for not being able to start is there is
     * already an existing migration in progress.
     */
    bool start(OperationContext* txn,
               const MigrationSessionId& sessionId,
               const std::string& ns,
               const BSONObj& min,
               const BSONObj& max,
               const BSONObj& shardKeyPattern);

    void done(OperationContext* txn);

    /**
     * If a migration for the chunk in 'ns' containing 'obj' is in progress, saves this insert
     * to the transfer mods log. The entries saved here are later transferred to the receiving
     * side of the migration.
     */
    void logInsertOp(OperationContext* txn,
                     const char* ns,
                     const BSONObj& obj,
                     bool notInActiveChunk);

    /**
     * If a migration for the chunk in 'ns' containing 'updatedDoc' is in progress, saves this
     * update to the transfer mods log. The entries saved here are later transferred to the
     * receiving side of the migration.
     */
    void logUpdateOp(OperationContext* txn,
                     const char* ns,
                     const BSONObj& updatedDoc,
                     bool notInActiveChunk);

    /**
     * If a migration for the chunk in 'ns' containing 'obj' is in progress, saves this delete
     * to the transfer mods log. The entries saved here are later transferred to the receiving
     * side of the migration.
     */
    void logDeleteOp(OperationContext* txn,
                     const char* ns,
                     const BSONObj& obj,
                     bool notInActiveChunk);

    /**
     * Determines whether the given document 'doc' in namespace 'ns' is within the range
     * of a currently migrating chunk.
     */
    bool isInMigratingChunk(const NamespaceString& ns, const BSONObj& doc);

    /**
     * Called from the source of a migration process, this method transfers the accummulated local
     * mods from source to destination.
     */
    bool transferMods(OperationContext* txn,
                      const MigrationSessionId& sessionId,
                      std::string& errmsg,
                      BSONObjBuilder& b);

    /**
     * Get the disklocs that belong to the chunk migrated and sort them in _cloneLocs (to avoid
     * seeking disk later).
     *
     * @param maxChunkSize number of bytes beyond which a chunk's base data (no indices) is
     *      considered too large to move
     * @param errmsg filled with textual description of error if this call return false
     *
     * Returns false if approximate chunk size is too big to move or true otherwise.
     */
    bool storeCurrentLocs(OperationContext* txn,
                          long long maxChunkSize,
                          std::string& errmsg,
                          BSONObjBuilder& result);

    bool clone(OperationContext* txn,
               const MigrationSessionId& sessionId,
               std::string& errmsg,
               BSONObjBuilder& result);

    void aboutToDelete(const RecordId& dl);

    std::size_t cloneLocsRemaining() const;

    long long mbUsed() const;

    bool getInCriticalSection() const;

    void setInCriticalSection(bool inCritSec);

    /**
     * Blocks until the "in critical section" state changes and returns true if we are NOT in the
     * critical section
     */
    bool waitTillNotInCriticalSection(int maxSecondsToWait);

    bool isActive() const;

private:
    friend class LogOpForShardingHandler;

    /**
     * Insert items from docIdList to a new array with the given fieldName in the given builder. If
     * explode is true, the inserted object will be the full version of the document. Note that
     * whenever an item from the docList is inserted to the array, it will also be removed from
     * docList.
     *
     * Should be holding the collection lock for ns if explode is true.
     */
    void _xfer(OperationContext* txn,
               const std::string& ns,
               Database* db,
               std::list<BSONObj>* docIdList,
               BSONObjBuilder& builder,
               const char* fieldName,
               long long& size,
               bool explode);

    NamespaceString _getNS() const;

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M)  Must hold _mutex for access.
    // (MG) For reads, _mutex *OR* Global IX Lock must be held.
    //      For writes, the _mutex *AND* (Global Shared or Exclusive Lock) must be held.
    // (C)  Must hold _cloneLocsMutex for access.
    //
    // Locking order:
    //
    // Global Lock -> _mutex -> _cloneLocsMutex

    mutable stdx::mutex _mutex;

    stdx::condition_variable _inCriticalSectionCV;  // (M)

    // Is migration currently in critical section. This can be used to block new writes.
    bool _inCriticalSection{false};  // (M)

    std::unique_ptr<PlanExecutor> _deleteNotifyExec;  // (M)

    // List of _id of documents that were modified that must be re-cloned.
    std::list<BSONObj> _reload;  // (M)

    // List of _id of documents that were deleted during clone that should be deleted later.
    std::list<BSONObj> _deleted;  // (M)

    // Bytes in _reload + _deleted
    long long _memoryUsed{0};  // (M)

    // Uniquely identifies a migration and indicates a migration is active when set.
    boost::optional<MigrationSessionId> _sessionId{boost::none};  // (MG)

    NamespaceString _nss;      // (MG)
    BSONObj _min;              // (MG)
    BSONObj _max;              // (MG)
    BSONObj _shardKeyPattern;  // (MG)

    mutable stdx::mutex _cloneLocsMutex;

    // List of record id that needs to be transferred from here to the other side.
    std::set<RecordId> _cloneLocs;  // (C)
};

}  // namespace mongo
