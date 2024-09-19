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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace CollectionValidation {

/**
 * Contains information about the collection being validated and the user provided validation
 * options. Additionally it maintains the state of shared objects throughtout the validation, such
 * as locking, cursors and data throttling.
 *
 * TODO (SERVER-93766): Do not subclass ValidationOptions. Going forward we want a clean separation
 * between immutable preconditions of the validation (which needs to be logged in advance), and
 * mutable runtime-state of the validation (which needs to be accounted for memory-limiting
 * purposes), at present ValidateState does both jobs, hence the best idiom ever:
 * subclass-which-takes-a-parent-instance-in-its-constructor paradigm.
 */
class ValidateState : public ValidationOptions {
    ValidateState(const ValidateState&) = delete;
    ValidateState& operator=(const ValidateState&) = delete;

public:
    ValidateState(OperationContext* opCtx, const NamespaceString& nss, ValidationOptions options);

    const NamespaceString& nss() const {
        return _nss;
    }

    bool shouldEnforceFastCount() const;

    BSONValidateModeEnum getBSONValidateMode() const {
        return isBSONConformanceValidation() ? BSONValidateModeEnum::kFull
                                             : BSONValidateModeEnum::kExtended;
    }

    bool isCollectionSchemaViolated() const {
        return _collectionSchemaViolated;
    }

    void setCollectionSchemaViolated() {
        _collectionSchemaViolated = true;
    }

    bool isTimeseriesDataInconsistent() const {
        return _timeseriesDataInconsistency;
    }

    void setTimeseriesDataInconsistent() {
        _timeseriesDataInconsistency = true;
    }

    bool isTimeseriesBucketingParametersChangedInconsistent() const {
        return _timeseriesBucketingParametersChangedInconsistent;
    }

    void setTimeseriesBucketingParametersChangedInconsistent() {
        _timeseriesBucketingParametersChangedInconsistent = true;
    }

    bool isBSONDataNonConformant() const {
        return _BSONDataNonConformant;
    }

    void setBSONDataNonConformant() {
        _BSONDataNonConformant = true;
    }

    UUID uuid() const {
        invariant(_uuid);
        return *_uuid;
    }

    const CollectionPtr& getCollection() const {
        invariant(_collection);
        return _collection;
    }

    const std::vector<std::string>& getIndexIdents() const {
        return _indexIdents;
    }

    /**
     * Map of index names to index cursors.
     */
    const StringMap<std::unique_ptr<SortedDataInterfaceThrottleCursor>>& getIndexCursors() const {
        return _indexCursors;
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& getTraverseRecordStoreCursor() const {
        return _traverseRecordStoreCursor;
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& getSeekRecordStoreCursor() const {
        return _seekRecordStoreCursor;
    }

    const StringMap<std::unique_ptr<ColumnStore::Cursor>>& getColumnStoreCursors() const {
        return _columnStoreIndexCursors;
    }

    RecordId getFirstRecordId() const {
        return _firstRecordId;
    }

    /**
     * Saves and restores the open cursors to release snapshots and minimize cache pressure for
     * validation.
     *
     * Throws on interruptions.
     */
    void yieldCursors(OperationContext* opCtx);

    /**
     * Obtains a collection consistent with the snapshot.
     */
    Status initializeCollection(OperationContext* opCtx);

    /**
     * Initializes all the cursors to be used during validation and moves the traversal record
     * store cursor to the first record.
     */
    void initializeCursors(OperationContext* opCtx);

    boost::optional<Timestamp> getValidateTimestamp() {
        return _validateTs;
    }

private:
    ValidateState() = delete;

    NamespaceString _nss;
    bool _collectionSchemaViolated = false;
    bool _timeseriesDataInconsistency = false;
    bool _timeseriesBucketingParametersChangedInconsistent = false;
    bool _BSONDataNonConformant = false;
    // To avoid racing with shutdown.
    boost::optional<Lock::GlobalLock> _globalLock;

    // Locks for foreground validation only.
    boost::optional<AutoGetDb> _databaseLock;
    boost::optional<CollectionNamespaceOrUUIDLock> _collectionLock;

    // Hold a reference to the CollectionCatalog for a collection instance at a point-in-time to
    // remain valid during the duration of background validation.
    std::shared_ptr<const CollectionCatalog> _catalog;
    CollectionPtr _collection;

    // Always present after construction, but needs to be boost::optional due to the lack of default
    // constructor
    boost::optional<UUID> _uuid;

    // Stores the index idents that are going to be validated.
    std::vector<std::string> _indexIdents;

    // Shared cursors to be used during validation, created in 'initializeCursors()'.
    StringMap<std::unique_ptr<SortedDataInterfaceThrottleCursor>> _indexCursors;
    std::unique_ptr<SeekableRecordThrottleCursor> _traverseRecordStoreCursor;
    std::unique_ptr<SeekableRecordThrottleCursor> _seekRecordStoreCursor;
    StringMap<std::unique_ptr<ColumnStore::Cursor>> _columnStoreIndexCursors;

    RecordId _firstRecordId;

    DataThrottle _dataThrottle;

    boost::optional<Timestamp> _validateTs = boost::none;
};

}  // namespace CollectionValidation
}  // namespace mongo
