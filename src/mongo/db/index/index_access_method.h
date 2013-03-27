/**
*    Copyright (C) 2013 10gen Inc.
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
*/

#pragma once

#include "mongo/db/diskloc.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    class UpdateTicket;
    class InsertDeleteOptions;

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
        virtual Status insert(const BSONObj& obj,
                              const DiskLoc& loc,
                              const InsertDeleteOptions& options,
                              int64_t* numInserted) = 0;

        /** 
         * Analogous to above, but remove the records instead of inserting them.  If not NULL,
         * numDeleted will be set to the number of keys removed from the index for the document.
         */
        virtual Status remove(const BSONObj& obj,
                              const DiskLoc& loc,
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
        virtual Status validateUpdate(const BSONObj& from,
                                      const BSONObj& to,
                                      const DiskLoc& loc,
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
        virtual Status update(const UpdateTicket& ticket) = 0;

        /**
         * Fills in '*out' with an IndexCursor.  Return a status indicating success or reason of
         * failure. If the latter, '*out' contains NULL.  See index_cursor.h for IndexCursor usage.
         */
        virtual Status newCursor(IndexCursor **out) = 0;

        /**
         * Try to page-in the pages that contain the keys generated from 'obj'.
         * This can be used to speed up future accesses to an index by trying to ensure the
         * appropriate pages are not swapped out.
         * See prefetch.cpp.
         */
        virtual Status touch(const BSONObj& obj) = 0;

        //
        // Bulk operations support (TODO)
        //

        // virtual Status insertBulk(BulkDocs arg) = 0;

        // virtual Status removeBulk(BulkDocs arg) = 0;
    };

    /**
     * Updates are two steps: verify that it's a valid update, and perform it.
     * validateUpdate fills out the UpdateStatus and update actually applies it.
     */
    class UpdateTicket {
    public:
        UpdateTicket() : _isValid(false), _isMultiKey(false) { }

        // Multikey is a bit set in the on-disk catalog.  If an update is multi-key we have
        // to set that bit.  We propagate this up so the caller can do that.
        bool isMultiKey() const { return _isMultiKey; }

    protected:
        // These friends are the classes that actually fill out an UpdateStatus.
        // TODO(hk): when we check in BtreeBasedAccessMethod, make friends.
        // template <class Key> friend class BtreeBasedAccessMethod;

        bool _isValid;
        bool _isMultiKey;

        // This is meant to be filled out only by the friends above.
        void *_indexSpecificUpdateData;
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
