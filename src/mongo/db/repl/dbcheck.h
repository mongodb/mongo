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

#include <memory>

#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/dbcheck_gen.h"
#include "mongo/util/md5.hpp"

namespace mongo {

// Forward declarations.
class Collection;
class CollectionPtr;
class OperationContext;

namespace repl {
class OpTime;
}

/**
 * Logs an entry into 'local.system.healthLog'.
 */
std::unique_ptr<HealthLogEntry> dbCheckHealthLogEntry(const boost::optional<NamespaceString>& nss,
                                                      SeverityEnum severity,
                                                      const std::string& msg,
                                                      OplogEntriesEnum operation,
                                                      const boost::optional<BSONObj>& data);

/**
 * Logs an error into 'local.system.healthLog'.
 */
std::unique_ptr<HealthLogEntry> dbCheckErrorHealthLogEntry(
    const boost::optional<NamespaceString>& nss,
    const std::string& msg,
    OplogEntriesEnum operation,
    const Status& err,
    const BSONObj& context = BSONObj());

std::unique_ptr<HealthLogEntry> dbCheckWarningHealthLogEntry(const NamespaceString& nss,
                                                             const std::string& msg,
                                                             OplogEntriesEnum operation,
                                                             const Status& err);
/**
 * Get a HealthLogEntry for a dbCheck batch.
 */
std::unique_ptr<HealthLogEntry> dbCheckBatchEntry(
    const NamespaceString& nss,
    int64_t count,
    int64_t bytes,
    const std::string& expectedHash,
    const std::string& foundHash,
    const BSONKey& minKey,
    const BSONKey& maxKey,
    const boost::optional<Timestamp>& timestamp,
    const repl::OpTime& optime,
    const boost::optional<CollectionOptions>& options = boost::none);

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
                  const CollectionPtr& collection,
                  const BSONKey& start,
                  const BSONKey& end,
                  int64_t maxCount = std::numeric_limits<int64_t>::max(),
                  int64_t maxBytes = std::numeric_limits<int64_t>::max());

    ~DbCheckHasher();

    /**
     * Hash all documents up to the deadline.
     */
    Status hashAll(OperationContext* opCtx, Date_t deadline = Date_t::max());

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

    DataCorruptionDetectionMode _previousDataCorruptionMode;
    PrepareConflictBehavior _previousPrepareConflictBehavior;
};

namespace repl {

/**
 * Perform the dbCheck oplog command.
 *
 * Returns a `Status` to match the type used for oplog command hooks, but in fact always handles
 * errors (primarily by writing to the health log), so always returns `Status::OK`.
 */
Status dbCheckOplogCommand(OperationContext* opCtx,
                           const repl::OplogEntry& entry,
                           OplogApplication::Mode mode);
}  // namespace repl
}  // namespace mongo
