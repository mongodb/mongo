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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/health_log_gen.h"
#include "mongo/db/local_catalog/throttle_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/dbcheck/dbcheck_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/md5.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// Forward declarations.
class ClusteredIndexSpec;
class Collection;
class CollectionPtr;
class OperationContext;

namespace repl {
class OpTime;
}

/**
 * Converts OplogEntriesEnum or DbCheckValidationModeEnum into string for health log.
 */
std::string renderForHealthLog(OplogEntriesEnum op);

/**
 * Logs an entry into 'local.system.healthLog'.
 *
 * The parameters nss and collectionUUID are boost::optional because they are not present in
 * DbCheckOplogStartStop health log entries. DbCheckOplogStartStop entries will use boost::none
 * for both nss and collectionUUID.
 */
std::unique_ptr<HealthLogEntry> dbCheckHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const boost::optional<NamespaceString>& nss,
    const boost::optional<UUID>& collectionUUID,
    SeverityEnum severity,
    const std::string& msg,
    ScopeEnum scope,
    OplogEntriesEnum operation,
    const boost::optional<BSONObj>& data);

/**
 * Logs an error into 'local.system.healthLog'.
 */
std::unique_ptr<HealthLogEntry> dbCheckErrorHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const boost::optional<NamespaceString>& nss,
    const boost::optional<UUID>& collectionUUID,
    const std::string& msg,
    ScopeEnum scope,
    OplogEntriesEnum operation,
    const Status& err,
    const BSONObj& context = BSONObj());

std::unique_ptr<HealthLogEntry> dbCheckWarningHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    const std::string& msg,
    ScopeEnum scope,
    OplogEntriesEnum operation,
    const Status& err,
    const BSONObj& context = BSONObj());
/**
 * Get a HealthLogEntry for a dbCheck batch.
 */
std::unique_ptr<HealthLogEntry> dbCheckBatchHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const boost::optional<UUID>& batchId,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    int64_t count,
    int64_t bytes,
    const std::string& expectedHash,
    const std::string& foundHash,
    const BSONObj& batchStart,
    const BSONObj& batchEnd,
    const BSONObj& lastKeyChecked,
    int64_t nConsecutiveIdenticalIndexKeysAtEnd,
    const boost::optional<Timestamp>& timestamp,
    const repl::OpTime& optime,
    const boost::optional<CollectionOptions>& options = boost::none,
    const boost::optional<BSONObj>& indexSpec = boost::none);

bool isIndexOrderAndUniquenessPreserved(const KeyStringEntry& curr,
                                        const KeyStringEntry& next,
                                        bool isUnique);

struct ReadSourceWithTimestamp {
    RecoveryUnit::ReadSource readSource;
    boost::optional<Timestamp> timestamp = boost::none;
};

/**
 * Helper for converting a keystring to its BSON format. Converting a keystring to BSON will remove
 * the RecordId, as BSON objects cannot store the RecordId. As a result, if you then convert the
 * BSON back to keystring format, that keystring will not have a RecordId.
 */
BSONObj _keyStringToBsonSafeHelper(const key_string::Value& keyString, const Ordering& ordering);

/**
 * Helper for getting the next distinct key in the index after currKeyStringBson.
 */
boost::optional<KeyStringEntry> _getNextDistinctKeyInIndex(
    OperationContext* opCtx,
    const std::unique_ptr<SortedDataInterfaceThrottleCursor>& indexCursor,
    const key_string::Version& version,
    const Ordering& ordering,
    const BSONObj& currKeyStringBson);

/**
 * DbCheckAcquisition is a helper class to acquire locks and set RecoveryUnit state for the dbCheck
 * operation.
 */
class DbCheckAcquisition {
public:
    explicit DbCheckAcquisition(OperationContext* opCtx,
                                const NamespaceString& nss,
                                ReadSourceWithTimestamp readSource,
                                PrepareConflictBehavior prepareConflictBehavior);

    ~DbCheckAcquisition();

private:
    OperationContext* _opCtx;

public:
    const ReadSourceScope readSourceScope;
    const PrepareConflictBehavior prevPrepareConflictBehavior;
    const DataCorruptionDetectionMode prevDataCorruptionMode;

private:
    boost::optional<CollectionAcquisition> _coll;

public:
    const CollectionAcquisition& collection() const {
        return *_coll;
    }
};

/**
 * Hashing collections or indexes.
 *
 * Provides MD5-based hashing of ranges of documents.  Note that this class does *not* provide
 * synchronization: clients must, for example, lock the database to ensure that named collections
 * exist, and hold at least a MODE_IS lock before asking a `DbCheckHasher` to retrieve any
 * documents.
 */
class DbCheckHasher {
public:
    /**
     * Create a new DbCheckHasher hashing documents or index keys within the given limits.
     *
     * The check will end when any of the specified limits are reached.  Must be called on a
     * collection with an _id index; otherwise a DBException with code ErrorCodes::IndexNotFound
     * will be thrown.
     *
     * @param collection The collection to hash from.
     * @param start The first key to hash (exclusive).
     * @param end The last key to hash (inclusive).
     * @param maxCount The maximum number of documents or index keys to hash.
     * @param maxBytes The maximum number of bytes to hash.
     * @param deadlineOnSecondary The time deadline to finish this batch by on the secondary.
     */
    DbCheckHasher(OperationContext* opCtx,
                  const DbCheckAcquisition& acquisition,
                  const BSONObj& start,
                  const BSONObj& end,
                  boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParameters,
                  DataThrottle* dataThrottle,
                  boost::optional<StringData> indexName = boost::none,
                  int64_t maxCount = std::numeric_limits<int64_t>::max(),
                  int64_t maxBytes = std::numeric_limits<int64_t>::max(),
                  Date_t deadlineOnSecondary = Date_t::max());

    ~DbCheckHasher() = default;

    /**
     * Hashes all documents up to the deadline.
     */
    Status hashForCollectionCheck(OperationContext* opCtx,
                                  const CollectionPtr& collPtr,
                                  Date_t deadlineOnPrimary = Date_t::max());

    /**
     * Hash index keys between first and last inclusive.
     * If nConsecutiveIdenticalKeysAtEnd > 1, this means that the last keystring is in a series
     * of consecutive identical keys, and we should only hash nConsecutiveIdenticalKeysAtEnd of
     * those.
     */
    Status hashForExtraIndexKeysCheck(OperationContext* opCtx,
                                      const Collection* collection,
                                      const BSONObj& batchStartBson,
                                      const BSONObj& batchEndBson,
                                      const BSONObj& lastKeyCheckedBson);

    /**
     * Checks if a document has missing index keys by finding the index keys that should be
     * generated for each document and checking that they actually exist in the index.
     */
    Status validateMissingKeys(OperationContext* opCtx,
                               BSONObj& currentObj,
                               RecordId& currentRecordId,
                               const CollectionPtr& collPtr);

    /**
     * Returns the total hash of all items seen so far.
     */
    std::string total(void);

    /**
     * Gets the last key this hasher has hashed.
     *
     * Again, not the same as the `end` passed in; this is MinKey if no items have been hashed.
     */
    BSONObj lastKeySeen(void) const;

    int64_t bytesSeen(void) const;

    int64_t docsSeen(void) const;

    int64_t keysSeen(void) const;

    int64_t countSeen(void) const;

    int64_t nConsecutiveIdenticalIndexKeysSeenAtEnd(void) const;

private:
    /**
     * Checks if we can hash `obj` without going over our limits for collection check.
     */
    bool _canHashForCollectionCheck(const BSONObj& obj);

    bool _isIdSameAsRecordId(const RecordId& rid,
                             const BSONObj& doc,
                             const ClusteredIndexSpec& indexSpec,
                             const CollatorInterface* collator);

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
    md5_state_t _state;

    BSONObj _maxKey;
    BSONObj _lastKeySeen = kMinBSONKey;

    boost::optional<StringData> _indexName;

    // Represents the max number of docs or keys seen, which varies based on the validation mode:
    //  - "dataConsistency": _countDocsSeen <= _maxCount
    //  - "dataConsistencyAndMissingIndexKeysCheck": (_countDocsSeen + _countKeysSeen) <= _maxCount
    //  - "extraIndexKeysCheck": _countKeysSeen <= _maxCount
    int64_t _maxCount = 0;
    int64_t _countDocsSeen = 0;
    int64_t _countKeysSeen = 0;

    int64_t _maxBytes = 0;
    int64_t _bytesSeen = 0;
    int64_t _nConsecutiveIdenticalIndexKeysSeenAtEnd = 0;

    std::vector<const IndexCatalogEntry*> _indexes;
    std::vector<BSONObj> _missingIndexKeys;

    boost::optional<SecondaryIndexCheckParameters> _secondaryIndexCheckParameters;
    DataThrottle* _dataThrottle;

    // Max time that the hasher should spend on a batch. This should only be populated on the
    // secondary since the primary has its own batch time limit.
    Date_t _deadlineOnSecondary;
};

namespace repl {

/**
 * Performs the dbCheck oplog command.
 *
 * Returns a `Status` to match the type used for oplog command hooks, but in fact always handles
 * errors (primarily by writing to the health log), so always returns `Status::OK`.
 */
Status dbCheckOplogCommand(OperationContext* opCtx,
                           const repl::OplogEntry& entry,
                           OplogApplication::Mode mode);
}  // namespace repl
}  // namespace mongo
