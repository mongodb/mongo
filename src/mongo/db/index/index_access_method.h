
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

#include <atomic>
#include <memory>
#include <set>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_metadata_access_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

class BSONObjBuilder;
class MatchExpression;
class UpdateTicket;
struct InsertResult;
struct InsertDeleteOptions;

bool failIndexKeyTooLongParam();

/**
 * An IndexAccessMethod is the interface through which all the mutation, lookup, and
 * traversal of index entries is done. The class is designed so that the underlying index
 * data structure is opaque to the caller.
 *
 * IndexAccessMethods for existing indices are obtained through the system catalog.
 *
 * We assume the caller has whatever locks required.  This interface is not thread safe.
 *
 */
class IndexAccessMethod {
    MONGO_DISALLOW_COPYING(IndexAccessMethod);

public:
    IndexAccessMethod() = default;
    virtual ~IndexAccessMethod() = default;

    //
    // Lookup, traversal, and mutation support
    //

    /**
     * Internally generate the keys {k1, ..., kn} for 'obj'.  For each key k, insert (k ->
     * 'loc') into the index.  'obj' is the object at the location 'loc'.
     * If 'result' is not null, 'numInserted' will be set to the number of keys added to the index
     * for the document and the number of duplicate keys will be appended to 'dupsInserted' if this
     * is a unique index and duplicates are allowed.
     *
     * If there is more than one key for 'obj', either all keys will be inserted or none will.
     *
     * The behavior of the insertion can be specified through 'options'.
     */
    virtual Status insert(OperationContext* opCtx,
                          const BSONObj& obj,
                          const RecordId& loc,
                          const InsertDeleteOptions& options,
                          InsertResult* result) = 0;

    virtual Status insertKeys(OperationContext* opCtx,
                              const BSONObjSet& keys,
                              const BSONObjSet& multikeyMetadataKeys,
                              const MultikeyPaths& multikeyPaths,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              InsertResult* result) = 0;

    /**
     * Analogous to above, but remove the records instead of inserting them.
     * 'numDeleted' will be set to the number of keys removed from the index for the document.
     */
    virtual Status remove(OperationContext* opCtx,
                          const BSONObj& obj,
                          const RecordId& loc,
                          const InsertDeleteOptions& options,
                          int64_t* numDeleted) = 0;

    virtual Status removeKeys(OperationContext* opCtx,
                              const BSONObjSet& keys,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numDeleted) = 0;

    /**
     * Checks whether the index entries for the document 'from', which is placed at location
     * 'loc' on disk, can be changed to the index entries for the doc 'to'. Provides a ticket
     * for actually performing the update.
     *
     * Returns an error if the update is invalid.  The ticket will also be marked as invalid.
     * Returns OK if the update should proceed without error.  The ticket is marked as valid.
     *
     * There is no obligation to perform the update after performing validation.
     */
    virtual Status validateUpdate(OperationContext* opCtx,
                                  const BSONObj& from,
                                  const BSONObj& to,
                                  const RecordId& loc,
                                  const InsertDeleteOptions& options,
                                  UpdateTicket* ticket,
                                  const MatchExpression* indexFilter) = 0;

    /**
     * Perform a validated update.  The keys for the 'from' object will be removed, and the keys
     * for the object 'to' will be added.  Returns OK if the update succeeded, failure if it did
     * not.  If an update does not succeed, the index will be unmodified, and the keys for
     * 'from' will remain.  Assumes that the index has not changed since validateUpdate was
     * called.  If the index was changed, we may return an error, as our ticket may have been
     * invalidated.
     *
     * 'numInserted' will be set to the number of keys inserted into the index for the document.
     * 'numDeleted' will be set to the number of keys removed from the index for the document.
     */
    virtual Status update(OperationContext* opCtx,
                          const UpdateTicket& ticket,
                          int64_t* numInserted,
                          int64_t* numDeleted) = 0;

    /**
     * Returns an unpositioned cursor over 'this' index.
     */
    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                                   bool isForward) const = 0;
    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(
        OperationContext* opCtx) const = 0;

    // ------ index level operations ------


    /**
     * initializes this index
     * only called once for the lifetime of the index
     * if called multiple times, is an error
     */
    virtual Status initializeAsEmpty(OperationContext* opCtx) = 0;

    /**
     * Try to page-in the pages that contain the keys generated from 'obj'.
     * This can be used to speed up future accesses to an index by trying to ensure the
     * appropriate pages are not swapped out.
     * See prefetch.cpp.
     */
    virtual Status touch(OperationContext* opCtx, const BSONObj& obj) = 0;

    /**
     * this pages in the entire index
     */
    virtual Status touch(OperationContext* opCtx) const = 0;

    /**
     * Walk the entire index, checking the internal structure for consistency.
     * Set numKeys to the number of keys in the index.
     */
    virtual void validate(OperationContext* opCtx,
                          int64_t* numKeys,
                          ValidateResults* fullResults) const = 0;

    /**
     * Add custom statistics about this index to BSON object builder, for display.
     *
     * 'scale' is a scaling factor to apply to all byte statistics.
     *
     * Returns true if stats were appended.
     */
    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* result,
                                   double scale) const = 0;

    /**
     * @return The number of bytes consumed by this index.
     *         Exactly what is counted is not defined based on padding, re-use, etc...
     */
    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const = 0;

    virtual RecordId findSingle(OperationContext* opCtx, const BSONObj& key) const = 0;

    /**
     * Attempt compaction to regain disk space if the indexed record store supports
     * compaction-in-place.
     */
    virtual Status compact(OperationContext* opCtx) = 0;

    /**
     * Sets this index as multikey with the provided paths.
     */
    virtual void setIndexIsMultikey(OperationContext* opCtx, MultikeyPaths paths) = 0;

    //
    // Bulk operations support
    //

    class BulkBuilder {
    public:
        using Sorter = mongo::Sorter<BSONObj, RecordId>;

        virtual ~BulkBuilder() = default;

        /**
         * Insert into the BulkBuilder as-if inserting into an IndexAccessMethod.
         */
        virtual Status insert(OperationContext* opCtx,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options) = 0;

        virtual const MultikeyPaths& getMultikeyPaths() const = 0;

        virtual bool isMultikey() const = 0;

        /**
         * Inserts all multikey metadata keys cached during the BulkBuilder's lifetime into the
         * underlying Sorter, finalizes it, and returns an iterator over the sorted dataset.
         */
        virtual Sorter::Iterator* done() = 0;

        /**
         * Returns number of keys inserted using this BulkBuilder.
         */
        virtual int64_t getKeysInserted() const = 0;
    };

    /**
     * Starts a bulk operation.
     * You work on the returned BulkBuilder and then call commitBulk.
     * This can return NULL, meaning bulk mode is not available.
     *
     * It is only legal to initiate bulk when the index is new and empty.
     *
     * maxMemoryUsageBytes: amount of memory consumed before the external sorter starts spilling to
     *                      disk
     */
    virtual std::unique_ptr<BulkBuilder> initiateBulk(size_t maxMemoryUsageBytes) = 0;

    /**
     * Call this when you are ready to finish your bulk work.
     * Pass in the BulkBuilder returned from initiateBulk.
     * @param bulk - Something created from initiateBulk
     * @param mayInterrupt - Is this commit interruptible (will cancel)
     * @param dupsAllowed - If false and 'dupRecords' is not null, append with the RecordIds of
     *                      the uninserted duplicates.
     *                      If true and 'dupKeys' is not null, append with the keys of the inserted
     *                      duplicates.
     * @param dupRecords - If not null, is filled with the RecordIds of uninserted duplicate keys.
     *                     If null, duplicate keys will return errors.
     * @param dupKeys - If not null and 'dupsAllowed' is true, is filled with the keys of inserted
     *                  duplicates.
     *                  If null, duplicates are inserted but not recorded.
     *
     * It is invalid and contradictory to pass both 'dupRecords' and 'dupKeys'.
     */

    virtual Status commitBulk(OperationContext* opCtx,
                              BulkBuilder* bulk,
                              bool dupsAllowed,
                              std::set<RecordId>* dupRecords,
                              std::vector<BSONObj>* dupKeys) = 0;

    /**
     * Specifies whether getKeys should relax the index constraints or not, in order of most
     * permissive to least permissive.
     */
    enum class GetKeysMode {
        // Relax all constraints.
        kRelaxConstraints,
        // Relax all constraints on documents that don't apply to a partial index.
        kRelaxConstraintsUnfiltered,
        // Enforce all constraints.
        kEnforceConstraints
    };

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     * Based on 'mode', it will honor or ignore index constraints, e.g. duplicated key, key too
     * long, and geo index parsing errors. The ignoring of constraints is for replication due to
     * idempotency reasons. In those cases, the generated 'keys' will be empty.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     *
     * If the 'multikeyMetadataKeys' pointer is non-null, then the function will populate the
     * BSONObjSet with any multikey metadata keys generated while processing the document. These
     * keys are not associated with the document itself, but instead represent multi-key path
     * information that must be stored in a reserved keyspace within the index.
     */
    virtual void getKeys(const BSONObj& obj,
                         GetKeysMode mode,
                         BSONObjSet* keys,
                         BSONObjSet* multikeyMetadataKeys,
                         MultikeyPaths* multikeyPaths) const = 0;

    /**
     * Given the set of keys, multikeyMetadataKeys and multikeyPaths generated by a particular
     * document, return 'true' if the index should be marked as multikey and 'false' otherwise.
     */
    virtual bool shouldMarkIndexAsMultikey(const BSONObjSet& keys,
                                           const BSONObjSet& multikeyMetadataKeys,
                                           const MultikeyPaths& multikeyPaths) const = 0;

    /**
     * Returns the intersection of 'fields' and the set of multikey metadata paths stored in the
     * index. Only index types which can store metadata describing an arbitrarily large set of
     * multikey paths need to override this method. Statistics reporting index seeks and keys
     * examined are written to 'stats'.
     */
    virtual std::set<FieldRef> getMultikeyPathSet(OperationContext*,
                                                  const stdx::unordered_set<std::string>& fields,
                                                  MultikeyMetadataAccessStats* stats) const {
        return {};
    }

    /**
     * Returns the set of all paths for which the index has multikey metadata keys. Only index types
     * which can store metadata describing an arbitrarily large set of multikey paths need to
     * override this method. Statistics reporting index seeks and keys examined are written to
     * 'stats'.
     */
    virtual std::set<FieldRef> getMultikeyPathSet(OperationContext* opCtx,
                                                  MultikeyMetadataAccessStats* stats) const {
        return {};
    }

    /**
     * Provides direct access to the SortedDataInterface. This should not be used to insert
     * documents into an index, except for testing purposes.
     */
    virtual SortedDataInterface* getSortedDataInterface() const = 0;
};

/**
 * Records number of keys inserted and duplicate keys inserted, if applicable.
 */
struct InsertResult {
public:
    std::int64_t numInserted{0};
    std::vector<BSONObj> dupsInserted;
};

/**
 * Updates are two steps: verify that it's a valid update, and perform it.
 * validateUpdate fills out the UpdateStatus and update actually applies it.
 */
class UpdateTicket {
public:
    UpdateTicket()
        : oldKeys(SimpleBSONObjComparator::kInstance.makeBSONObjSet()),
          newKeys(oldKeys),
          newMultikeyMetadataKeys(newKeys) {}

private:
    friend class AbstractIndexAccessMethod;

    bool _isValid{false};

    BSONObjSet oldKeys;
    BSONObjSet newKeys;

    BSONObjSet newMultikeyMetadataKeys;

    std::vector<BSONObj> removed;
    std::vector<BSONObj> added;

    RecordId loc;
    bool dupsAllowed;

    // Holds the path components that would cause this index to be multikey as a result of inserting
    // 'newKeys'. The 'newMultikeyPaths' vector remains empty if this index doesn't support
    // path-level multikey tracking.
    MultikeyPaths newMultikeyPaths;
};

/**
 * Flags we can set for inserts and deletes (and updates, which are kind of both).
 */
struct InsertDeleteOptions {
    // If there's an error, log() it.
    bool logIfError = false;

    // Are duplicate keys allowed in the index?
    bool dupsAllowed = false;

    // Only an index builder is allowed to insert into the index while it is building, so only the
    // index builder should set this to 'true'.
    bool fromIndexBuilder = false;

    // Should we relax the index constraints?
    IndexAccessMethod::GetKeysMode getKeysMode =
        IndexAccessMethod::GetKeysMode::kEnforceConstraints;
};

/**
 * Provides implementations for many functions in the IndexAccessMethod interface that will be
 * shared across concrete implementations.
 *
 * IndexCatalogEntry owns an instance of IndexAccessMethod; an IndexCatalogEntry is also required
 * for the initialization and core functionality of this abstract class. To avoid any circular
 * dependencies, it is important that IndexAccessMethod remain an interface.
 */
class AbstractIndexAccessMethod : public IndexAccessMethod {
    MONGO_DISALLOW_COPYING(AbstractIndexAccessMethod);

public:
    /**
     * Splits the sets 'left' and 'right' into two vectors, the first containing the elements that
     * only appeared in 'left', and the second containing only elements that appeared in 'right'.
     *
     * Note this considers objects which are not identical as distinct objects. For example,
     * setDifference({BSON("a" << 0.0)}, {BSON("a" << 0LL)}) would result in the pair
     * ( {BSON("a" << 0.0)}, {BSON("a" << 0LL)} ).
     */
    static std::pair<std::vector<BSONObj>, std::vector<BSONObj>> setDifference(
        const BSONObjSet& left, const BSONObjSet& right);

    AbstractIndexAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree);

    Status insert(OperationContext* opCtx,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  InsertResult* result) final;

    Status insertKeys(OperationContext* opCtx,
                      const BSONObjSet& keys,
                      const BSONObjSet& multikeyMetadataKeys,
                      const MultikeyPaths& multikeyPaths,
                      const RecordId& loc,
                      const InsertDeleteOptions& options,
                      InsertResult* result) final;

    Status remove(OperationContext* opCtx,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  int64_t* numDeleted) final;

    Status removeKeys(OperationContext* opCtx,
                      const BSONObjSet& keys,
                      const RecordId& loc,
                      const InsertDeleteOptions& options,
                      int64_t* numDeleted) final;

    Status validateUpdate(OperationContext* opCtx,
                          const BSONObj& from,
                          const BSONObj& to,
                          const RecordId& loc,
                          const InsertDeleteOptions& options,
                          UpdateTicket* ticket,
                          const MatchExpression* indexFilter) final;

    Status update(OperationContext* opCtx,
                  const UpdateTicket& ticket,
                  int64_t* numInserted,
                  int64_t* numDeleted) final;

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool isForward) const final;
    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx) const final;

    Status initializeAsEmpty(OperationContext* opCtx) final;

    Status touch(OperationContext* opCtx, const BSONObj& obj) final;

    Status touch(OperationContext* opCtx) const final;

    void validate(OperationContext* opCtx,
                  int64_t* numKeys,
                  ValidateResults* fullResults) const final;

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* result,
                           double scale) const final;

    long long getSpaceUsedBytes(OperationContext* opCtx) const final;

    RecordId findSingle(OperationContext* opCtx, const BSONObj& key) const final;

    Status compact(OperationContext* opCtx) final;

    void setIndexIsMultikey(OperationContext* opCtx, MultikeyPaths paths) final;

    std::unique_ptr<BulkBuilder> initiateBulk(size_t maxMemoryUsageBytes) final;

    Status commitBulk(OperationContext* opCtx,
                      BulkBuilder* bulk,
                      bool dupsAllowed,
                      std::set<RecordId>* dupRecords,
                      std::vector<BSONObj>* dupKeys) final;

    void getKeys(const BSONObj& obj,
                 GetKeysMode mode,
                 BSONObjSet* keys,
                 BSONObjSet* multikeyMetadataKeys,
                 MultikeyPaths* multikeyPaths) const final;

    bool shouldMarkIndexAsMultikey(const BSONObjSet& keys,
                                   const BSONObjSet& multikeyMetadataKeys,
                                   const MultikeyPaths& multikeyPaths) const override;

    SortedDataInterface* getSortedDataInterface() const override final;

protected:
    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     *
     * If the 'multikeyMetadataKeys' pointer is non-null, then the function will populate the
     * BSONObjSet with any multikey metadata keys generated while processing the document. These
     * keys are not associated with the document itself, but instead represent multi-key path
     * information that must be stored in a reserved keyspace within the index.
     */
    virtual void doGetKeys(const BSONObj& obj,
                           BSONObjSet* keys,
                           BSONObjSet* multikeyMetadataKeys,
                           MultikeyPaths* multikeyPaths) const = 0;

    IndexCatalogEntry* const _btreeState;  // owned by IndexCatalogEntry
    const IndexDescriptor* const _descriptor;

private:
    class BulkBuilderImpl;

    /**
     * Determine whether the given Status represents an exception that should cause the indexing
     * process to abort. The 'key' argument is passed in to allow the offending entry to be logged
     * in the event that a non-fatal 'ErrorCodes::DuplicateKeyValue' is encountered during a
     * background index build.
     */
    bool isFatalError(OperationContext* opCtx, Status status, BSONObj key);

    /**
     * Determines whether it's OK to ignore ErrorCodes::KeyTooLong for this OperationContext
     * TODO SERVER-36385: Remove this function.
     */
    bool ignoreKeyTooLong();

    /**
     * If true, we should check whether the index key exceeds the hardcoded limit.
     * TODO SERVER-36385: Remove this function.
     */
    bool shouldCheckIndexKeySize(OperationContext* opCtx);

    /**
     * Removes a single key from the index.
     *
     * Used by remove() only.
     */
    void removeOneKey(OperationContext* opCtx,
                      const BSONObj& key,
                      const RecordId& loc,
                      bool dupsAllowed);

    const std::unique_ptr<SortedDataInterface> _newInterface;
};

}  // namespace mongo
