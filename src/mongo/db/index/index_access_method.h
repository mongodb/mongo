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

#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"

namespace mongo {

    class BSONObjBuilder;
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
    public:
        virtual ~IndexAccessMethod() { }

        //
        // Lookup, traversal, and mutation support
        //

        /**
         * Internally generate the keys {k1, ..., kn} for 'obj'.  For each key k, insert (k ->
         * 'loc') into the index.  'obj' is the object at the location 'loc'.  If not NULL,
         * 'numInserted' will be set to the number of keys added to the index for the document.  If
         * there is more than one key for 'obj', either all keys will be inserted or none will.
         *
         * The behavior of the insertion can be specified through 'options'.
         */
        virtual Status insert(OperationContext* txn,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted) = 0;

        /**
         * Analogous to above, but remove the records instead of inserting them.  If not NULL,
         * numDeleted will be set to the number of keys removed from the index for the document.
         */
        virtual Status remove(OperationContext* txn,
                              const BSONObj& obj,
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
        virtual Status validateUpdate(OperationContext* txn,
                                      const BSONObj& from,
                                      const BSONObj& to,
                                      const RecordId& loc,
                                      const InsertDeleteOptions& options,
                                      UpdateTicket* ticket) = 0;

        /**
         * Perform a validated update.  The keys for the 'from' object will be removed, and the keys
         * for the object 'to' will be added.  Returns OK if the update succeeded, failure if it did
         * not.  If an update does not succeed, the index will be unmodified, and the keys for
         * 'from' will remain.  Assumes that the index has not changed since validateUpdate was
         * called.  If the index was changed, we may return an error, as our ticket may have been
         * invalidated.
         */
        virtual Status update(OperationContext* txn,
                              const UpdateTicket& ticket,
                              int64_t* numUpdated) = 0;

        /**
         * Fills in '*out' with an IndexCursor.  Return a status indicating success or reason of
         * failure. If the latter, '*out' contains NULL.  See index_cursor.h for IndexCursor usage.
         */
        virtual Status newCursor(OperationContext* txn, const CursorOptions& opts, IndexCursor** out) const = 0;

        // ------ index level operations ------


        /**
         * initializes this index
         * only called once for the lifetime of the index
         * if called multiple times, is an error
         */
        virtual Status initializeAsEmpty(OperationContext* txn) = 0;

        /**
         * Try to page-in the pages that contain the keys generated from 'obj'.
         * This can be used to speed up future accesses to an index by trying to ensure the
         * appropriate pages are not swapped out.
         * See prefetch.cpp.
         */
        virtual Status touch(OperationContext* txn, const BSONObj& obj) = 0;

        /**
         * this pages in the entire index
         */
        virtual Status touch(OperationContext* txn) const = 0;

        /**
         * Walk the entire index, checking the internal structure for consistency.
         * Set numKeys to the number of keys in the index.
         *
         * 'output' is used to store results of validate when 'full' is true.
         * If 'full' is false, 'output' may be NULL.
         *
         * Return OK if the index is valid.
         *
         * Currently wasserts that the index is invalid.  This could/should be changed in
         * the future to return a Status.
         */
        virtual Status validate(OperationContext* txn, bool full, int64_t* numKeys,
                                BSONObjBuilder* output) = 0;

        /**
         * Add custom statistics about this index to BSON object builder, for display.
         *
         * 'scale' is a scaling factor to apply to all byte statistics.
         */
        virtual void appendCustomStats(OperationContext* txn, BSONObjBuilder* result, double scale)
            const = 0;

        /**
         * @return The number of bytes consumed by this index.
         *         Exactly what is counted is not defined based on padding, re-use, etc...
         */
        virtual long long getSpaceUsedBytes( OperationContext* txn ) const = 0;

        //
        // Bulk operations support
        //

        /**
         * Starts a bulk operation.
         * You work on the returned IndexAccessMethod and then call commitBulk.
         * This can return NULL, meaning bulk mode is not available.
         *
         * Long term, you'll eventually be able to mix/match bulk, not bulk,
         * have as many as you want, etc..
         *
         * Caller owns the returned IndexAccessMethod.
         *
         * The provided OperationContext must outlive the IndexAccessMethod returned.
         *
         * For now (1/8/14) you can only do bulk when the index is empty
         * it will fail if you try other times.
         */
        virtual IndexAccessMethod* initiateBulk(OperationContext* txn) = 0;

        /**
         * Call this when you are ready to finish your bulk work.
         * Pass in the IndexAccessMethod gotten from initiateBulk.
         * After this method is called, the bulk index access method is invalid
         * and should not be used.
         * @param bulk - something created from initiateBulk
         * @param mayInterrupt - is this commit interruptable (will cancel)
         * @param dupsAllowed - if false, error or fill 'dups' if any duplicate values are found
         * @param dups - if NULL, error out on dups if not allowed
         *               if not NULL, put the bad RecordIds there
         */
        virtual Status commitBulk( IndexAccessMethod* bulk,
                                   bool mayInterrupt,
                                   bool dupsAllowed,
                                   std::set<RecordId>* dups ) = 0;
    };

    /**
     * Updates are two steps: verify that it's a valid update, and perform it.
     * validateUpdate fills out the UpdateStatus and update actually applies it.
     */
    class UpdateTicket {
    public:
        UpdateTicket() : _isValid(false) { }

    protected:
        // These friends are the classes that actually fill out an UpdateStatus.
        friend class BtreeBasedAccessMethod;

        class PrivateUpdateData;

        bool _isValid;

        // This is meant to be filled out only by the friends above.
        scoped_ptr<PrivateUpdateData> _indexSpecificUpdateData;
    };

    class UpdateTicket::PrivateUpdateData {
    public:
        virtual ~PrivateUpdateData() { }
    };

    /**
     * Flags we can set for inserts and deletes (and updates, which are kind of both).
     */
    struct InsertDeleteOptions {
        InsertDeleteOptions() : logIfError(false), dupsAllowed(false) { }

        // If there's an error, log() it.
        bool logIfError;

        // Are duplicate keys allowed in the index?
        bool dupsAllowed;
    };

}  // namespace mongo
