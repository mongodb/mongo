/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/dbcheck_gen.h"
#include "mongo/util/md5.hpp"

namespace mongo {

// Forward declarations.
class Collection;
class OperationContext;

namespace repl {
class OpTime;
}

/**
 * Get an error message if the check fails.
 */
std::unique_ptr<HealthLogEntry> dbCheckErrorHealthLogEntry(const NamespaceString& nss,
                                                           const std::string& msg,
                                                           OplogEntriesEnum operation,
                                                           const Status& err);

/**
 * Get a HealthLogEntry for a dbCheck batch.
 */
std::unique_ptr<HealthLogEntry> dbCheckBatchEntry(const NamespaceString& nss,
                                                  int64_t count,
                                                  int64_t bytes,
                                                  const std::string& expectedHash,
                                                  const std::string& foundHash,
                                                  const BSONKey& minKey,
                                                  const BSONKey& maxKey,
                                                  const repl::OpTime& optime);

/**
 * The collection metadata dbCheck sends between nodes.
 */
struct DbCheckCollectionInformation {
    std::string collectionName;
    boost::optional<UUID> prev;
    boost::optional<UUID> next;
    std::vector<BSONObj> indexes;
    BSONObj options;
};

/**
 * Get a HealthLogEntry for a dbCheck collection.
 */
std::unique_ptr<HealthLogEntry> dbCheckCollectionEntry(const NamespaceString& nss,
                                                       const UUID& uuid,
                                                       const DbCheckCollectionInformation& expected,
                                                       const DbCheckCollectionInformation& found,
                                                       const repl::OpTime& optime);

/**
 * Hashing collections and plans.
 *
 * Provides MD5-based hashing of ranges of documents.  Note that this class does *not* provide
 * synchronization: clients must, for example, lock the database to ensure that named collections
 * exist, and hold at least a MODE_IS lock before asking a `DbCheckHasher` to retrieve any
 * documents.
 */
class DbCheckHasher {
public:
    /**
     * Create a new DbCheckHasher hashing documents within the given limits.
     *
     * The check will end when any of the specified limits are reached.  Must be called on a
     * collection with an _id index; otherwise a DBException with code ErrorCodes::IndexNotFound
     * will be thrown.
     *
     * @param collection The collection to hash from.
     * @param start The first key to hash (exclusive).
     * @param end The last key to hash (inclusive).
     * @param maxCount The maximum number of documents to hash.
     * @param maxBytes The maximum number of bytes to hash.
     */
    DbCheckHasher(OperationContext* opCtx,
                  Collection* collection,
                  const BSONKey& start,
                  const BSONKey& end,
                  int64_t maxCount = std::numeric_limits<int64_t>::max(),
                  int64_t maxBytes = std::numeric_limits<int64_t>::max());

    /**
     * Hash all of our documents.
     */
    Status hashAll(void);

    /**
     * Return the total hash of all documents seen so far.
     */
    std::string total(void);

    /**
     * Get the last key this hasher has hashed.
     *
     * Again, not the same as the `end` passed in; this is MinKey if no documents have been hashed.
     */
    BSONKey lastKey(void) const;

    int64_t bytesSeen(void) const;

    int64_t docsSeen(void) const;

private:
    /**
     * Can we hash `obj` without going over our limits?
     */
    bool _canHash(const BSONObj& obj);

    OperationContext* _opCtx;
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
    md5_state_t _state;

    BSONKey _maxKey;
    BSONKey _last = BSONKey::min();

    int64_t _maxCount = 0;
    int64_t _countSeen = 0;

    int64_t _maxBytes = 0;
    int64_t _bytesSeen = 0;
};

/**
 * Get the given database in MODE_S, while also blocking stepdown (SERVER-28544) and allowing writes
 * to "local".
 */
class AutoGetDbForDbCheck {
public:
    AutoGetDbForDbCheck(OperationContext* opCtx, const NamespaceString& nss);

    Database* getDb(void) {
        return agd.getDb();
    }

private:
    Lock::DBLock localLock;
    AutoGetDb agd;
};

/**
 * Get the given collection in MODE_S, except that if the collection is missing it will report that
 * to the health log, and it takes an IX lock on "local" as a workaround to SERVER-28544 and to
 * ensure correct flush lock acquisition for MMAPV1.
 */
class AutoGetCollectionForDbCheck {
public:
    AutoGetCollectionForDbCheck(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const OplogEntriesEnum& type);
    Collection* getCollection(void) {
        return _collection;
    }

private:
    AutoGetDbForDbCheck _agd;
    Lock::CollectionLock _collLock;
    Collection* _collection;
};


/**
 * Gather the index information for a collection.
 */
std::vector<BSONObj> collectionIndexInfo(OperationContext* opCtx, Collection* collection);

/**
 * Gather other information for a collection.
 */
BSONObj collectionOptions(OperationContext* opCtx, Collection* collection);

namespace repl {

/**
 * Perform the dbCheck oplog command.
 *
 * Returns a `Status` to match the type used for oplog command hooks, but in fact always handles
 * errors (primarily by writing to the health log), so always returns `Status::OK`.
 */
Status dbCheckOplogCommand(OperationContext* opCtx,
                           const char* ns,
                           const BSONElement& ui,
                           BSONObj& cmd,
                           const repl::OpTime& optime,
                           OplogApplication::Mode mode);
}
}
