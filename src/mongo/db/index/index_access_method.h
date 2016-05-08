/**
*    Copyright (C) 2013-2014 MongoDB Inc.
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

#include <atomic>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

extern std::atomic<bool> failIndexKeyTooLong;  // NOLINT

class BSONObjBuilder;
class MatchExpression;
class UpdateTicket;
struct InsertDeleteOptions;

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
    IndexAccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree);
    virtual ~IndexAccessMethod() {}

    //
    // Lookup, traversal, and mutation support
    //

    /**
     * Internally generate the keys {k1, ..., kn} for 'obj'.  For each key k, insert (k ->
     * 'loc') into the index.  'obj' is the object at the location 'loc'.
     * 'numInserted' will be set to the number of keys added to the index for the document.  If
     * there is more than one key for 'obj', either all keys will be inserted or none will.
     *
     * The behavior of the insertion can be specified through 'options'.
     */
    Status insert(OperationContext* txn,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  int64_t* numInserted);

    /**
     * Analogous to above, but remove the records instead of inserting them.
     * 'numDeleted' will be set to the number of keys removed from the index for the document.
     */
    Status remove(OperationContext* txn,
                  const BSONObj& obj,
                  const RecordId& loc,
                  const InsertDeleteOptions& options,
                  int64_t* numDeleted);

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
    Status validateUpdate(OperationContext* txn,
                          const BSONObj& from,
                          const BSONObj& to,
                          const RecordId& loc,
                          const InsertDeleteOptions& options,
                          UpdateTicket* ticket,
                          const MatchExpression* indexFilter);

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
    Status update(OperationContext* txn,
                  const UpdateTicket& ticket,
                  int64_t* numInserted,
                  int64_t* numDeleted);

    /**
     * Returns an unpositioned cursor over 'this' index.
     */
    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* txn,
                                                           bool isForward = true) const;
    /**
     * Returns a pseudo-random cursor over 'this' index.
     */
    std::unique_ptr<SortedDataInterface::Cursor> newRandomCursor(OperationContext* txn) const;

    // ------ index level operations ------


    /**
     * initializes this index
     * only called once for the lifetime of the index
     * if called multiple times, is an error
     */
    Status initializeAsEmpty(OperationContext* txn);

    /**
     * Try to page-in the pages that contain the keys generated from 'obj'.
     * This can be used to speed up future accesses to an index by trying to ensure the
     * appropriate pages are not swapped out.
     * See prefetch.cpp.
     */
    Status touch(OperationContext* txn, const BSONObj& obj);

    /**
     * this pages in the entire index
     */
    Status touch(OperationContext* txn) const;

    /**
     * Walk the entire index, checking the internal structure for consistency.
     * Set numKeys to the number of keys in the index.

     * Return OK if the index is valid.
     */
    Status validate(OperationContext* txn, int64_t* numKeys, ValidateResults* fullResults);

    /**
     * Add custom statistics about this index to BSON object builder, for display.
     *
     * 'scale' is a scaling factor to apply to all byte statistics.
     *
     * Returns true if stats were appended.
     */
    bool appendCustomStats(OperationContext* txn, BSONObjBuilder* result, double scale) const;

    /**
     * @return The number of bytes consumed by this index.
     *         Exactly what is counted is not defined based on padding, re-use, etc...
     */
    long long getSpaceUsedBytes(OperationContext* txn) const;

    RecordId findSingle(OperationContext* txn, const BSONObj& key) const;

    /**
     * Attempt compaction to regain disk space if the indexed record store supports
     * compaction-in-place.
     */
    Status compact(OperationContext* txn);

    //
    // Bulk operations support
    //

    class BulkBuilder {
    public:
        /**
         * Insert into the BulkBuilder as-if inserting into an IndexAccessMethod.
         */
        Status insert(OperationContext* txn,
                      const BSONObj& obj,
                      const RecordId& loc,
                      const InsertDeleteOptions& options,
                      int64_t* numInserted);

    private:
        friend class IndexAccessMethod;

        using Sorter = mongo::Sorter<BSONObj, RecordId>;

        BulkBuilder(const IndexAccessMethod* index, const IndexDescriptor* descriptor);

        std::unique_ptr<Sorter> _sorter;
        const IndexAccessMethod* _real;
        int64_t _keysInserted = 0;

        // Set to true if at least one document causes IndexAccessMethod::getKeys() to return a
        // BSONObjSet with size strictly greater than one.
        bool _everGeneratedMultipleKeys = false;

        // Holds the path components that cause this index to be multikey. The '_indexMultikeyPaths'
        // vector remains empty if this index doesn't support path-level multikey tracking.
        MultikeyPaths _indexMultikeyPaths;
    };

    /**
     * Starts a bulk operation.
     * You work on the returned BulkBuilder and then call commitBulk.
     * This can return NULL, meaning bulk mode is not available.
     *
     * It is only legal to initiate bulk when the index is new and empty.
     */
    std::unique_ptr<BulkBuilder> initiateBulk();

    /**
     * Call this when you are ready to finish your bulk work.
     * Pass in the BulkBuilder returned from initiateBulk.
     * @param bulk - something created from initiateBulk
     * @param mayInterrupt - is this commit interruptable (will cancel)
     * @param dupsAllowed - if false, error or fill 'dups' if any duplicate values are found
     * @param dups - if NULL, error out on dups if not allowed
     *               if not NULL, put the bad RecordIds there
     */
    Status commitBulk(OperationContext* txn,
                      std::unique_ptr<BulkBuilder> bulk,
                      bool mayInterrupt,
                      bool dupsAllowed,
                      std::set<RecordId>* dups);

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     */
    virtual void getKeys(const BSONObj& obj,
                         BSONObjSet* keys,
                         MultikeyPaths* multikeyPaths) const = 0;

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

protected:
    // Determines whether it's OK to ignore ErrorCodes::KeyTooLong for this OperationContext
    bool ignoreKeyTooLong(OperationContext* txn);

    IndexCatalogEntry* _btreeState;  // owned by IndexCatalogEntry
    const IndexDescriptor* _descriptor;

private:
    void removeOneKey(OperationContext* txn,
                      const BSONObj& key,
                      const RecordId& loc,
                      bool dupsAllowed);

    const std::unique_ptr<SortedDataInterface> _newInterface;
};

/**
 * Updates are two steps: verify that it's a valid update, and perform it.
 * validateUpdate fills out the UpdateStatus and update actually applies it.
 */
class UpdateTicket {
    // No public interface
private:
    friend class IndexAccessMethod;

    bool _isValid;

    BSONObjSet oldKeys;
    BSONObjSet newKeys;

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
    InsertDeleteOptions() : logIfError(false), dupsAllowed(false) {}

    // If there's an error, log() it.
    bool logIfError;

    // Are duplicate keys allowed in the index?
    bool dupsAllowed;
};

}  // namespace mongo
