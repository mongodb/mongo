/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/uuid.h"

namespace mongo {

class Database;
class IndexCatalogEntry;

namespace CollectionValidation {

/**
 * Contains information about the collection being validated and the user provided validation
 * options. Additionally it maintains the state of shared objects throughtout the validation, such
 * as locking, cursors and data throttling.
 */
class ValidateState {
    ValidateState(const ValidateState&) = delete;
    ValidateState& operator=(const ValidateState&) = delete;

public:
    ValidateState(OperationContext* opCtx,
                  const NamespaceString& nss,
                  bool background,
                  bool fullValidate);

    const NamespaceString& nss() const {
        return _nss;
    }

    bool isBackground() const {
        return _background;
    }

    bool isFullValidate() const {
        return _fullValidate;
    }

    const UUID uuid() const {
        invariant(_uuid);
        return *_uuid;
    }

    const Database* getDatabase() const {
        invariant(_database);
        return _database;
    }

    Collection* getCollection() const {
        invariant(_collection);
        return _collection;
    }

    const std::vector<std::shared_ptr<const IndexCatalogEntry>>& getIndexes() const {
        return _indexes;
    }

    /**
     * Map of index names to index cursors.
     */
    const std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>>&
    getIndexCursors() const {
        return _indexCursors;
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& getTraverseRecordStoreCursor() const {
        return _traverseRecordStoreCursor;
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& getSeekRecordStoreCursor() const {
        return _seekRecordStoreCursor;
    }

    RecordId getFirstRecordId() const {
        return _firstRecordId;
    }

    /**
     * Yields both the database and collection locks temporarily in order to allow concurrent DDL
     * operations to passthrough. After both the database and collection locks have been restored,
     * check if validation can resume. Validation cannot be resumed if the database or collection is
     * dropped. In addition, if any indexes that were being validated are removed, validation will
     * be interrupted. A collection that was renamed across the same database can continue to be
     * validated, but a cross database collection rename will interrupt validation. If the locks
     * cannot be re-acquired, then this is guranteed to throw.
     *
     * Before yielding locks:
     *     - Save all the SortedDataInterface and SeekableRecord cursors.
     *
     * After locks are reacquired:
     *     - Check if the database exists.
     *     - Check if the collection exists.
     *     - Check if any indexes that were being validated have been removed.
     *     - Restore all the SortedDataInterface and SeekableRecord cursors.
     */
    void yieldLocks(OperationContext* opCtx);

    /**
     * Initializes all the cursors to be used during validation and moves the traversal record
     * store cursor to the first record. For background validation, this should be called while
     * holding the checkpoint lock when performing a background validation.
     */
    void initializeCursors(OperationContext* opCtx);

private:
    ValidateState() = delete;

    /**
     * Re-locks the database and collection with the appropriate locks for background validation.
     * This should only be called when '_background' is set to true.
     */
    void _relockDatabaseAndCollection(OperationContext* opCtx);

    NamespaceString _nss;
    bool _background;
    bool _fullValidate;
    OptionalCollectionUUID _uuid;

    boost::optional<AutoGetDb> _databaseLock;
    boost::optional<Lock::CollectionLock> _collectionLock;

    Database* _database;
    Collection* _collection;

    // Stores the indexes that are going to be validated. When validate yields periodically we'll
    // use this list to determine if validation should abort when an existing index that was
    // being validated is dropped. Additionally we'll use this list to determine which indexes to
    // skip during validation that may have been created in-between yields.
    std::vector<std::shared_ptr<const IndexCatalogEntry>> _indexes;

    // Shared cursors to be used during validation, created in 'initializeCursors()'.
    std::map<std::string, std::unique_ptr<SortedDataInterfaceThrottleCursor>> _indexCursors;
    std::unique_ptr<SeekableRecordThrottleCursor> _traverseRecordStoreCursor;
    std::unique_ptr<SeekableRecordThrottleCursor> _seekRecordStoreCursor;

    RecordId _firstRecordId;

    DataThrottle _dataThrottle;
};

}  // namespace CollectionValidation

}  // namespace mongo
