/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/bson/ordering.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/structure/head_manager.h"
#include "mongo/db/structure/record_store.h"

#pragma once

namespace mongo {

    class BucketDeletionNotification;

    /**
     * A version-hiding wrapper around the bulk builder for the Btree.
     */
    class BtreeBuilderInterface {
    public:
        virtual ~BtreeBuilderInterface() { }

        /**
         * Adds 'key' to intermediate storage.
         *
         * 'key' must be > or >= the last key passed to this function (depends on _dupsAllowed).  If
         * this is violated an error Status (ErrorCodes::InternalError) will be returned.
         */
        virtual Status addKey(const BSONObj& key, const DiskLoc& loc) = 0;

        /**
         * commit work.  if not called, destructor will clean up partially completed work
         *  (in case exception has happened).
         *
         * Returns number of keys added.
         */
        virtual unsigned long long commit(bool mayInterrupt) = 0;
    };

    /**
     * This is the interface for interacting with the Btree.  The index access and catalog layers
     * should use this.
     */
    class BtreeInterface {
    public:

        virtual ~BtreeInterface() { }

        /**
         * Interact with the Btree through the BtreeInterface.
         *
         * Does not own headManager.
         * Does not own recordStore.
         * Copies ordering.
         */
        static BtreeInterface* getInterface(HeadManager* headManager,
                                            RecordStore* recordStore,
                                            const Ordering& ordering,
                                            const string& indexName,
                                            int version,
                                            BucketDeletionNotification* bucketDeletion);

        //
        // Data changes
        //

        /**
         * Caller owns returned pointer.
         * 'this' must outlive the returned pointer.
         */
        virtual BtreeBuilderInterface* getBulkBuilder(OperationContext* txn,
                                                      bool dupsAllowed) = 0;

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const DiskLoc& loc,
                              bool dupsAllowed) = 0;

        virtual bool unindex(OperationContext* txn,
                             const BSONObj& key,
                             const DiskLoc& loc) = 0;

        // TODO: Hide this by exposing an update method?
        virtual Status dupKeyCheck(OperationContext* txn,
                                   const BSONObj& key,
                                   const DiskLoc& loc) = 0;

        //
        // Information about the tree
        //

        // TODO: expose full set of args for testing?
        virtual void fullValidate(OperationContext* txn, long long* numKeysOut) = 0;

        virtual bool isEmpty() = 0;
        
        /**
         * Attempt to bring whole index into memory. No-op is ok if not supported.
         */
        virtual Status touch(OperationContext* txn) const = 0;

        //
        // Navigation
        //

        class Cursor {
        public:
            virtual ~Cursor() {}

            virtual int getDirection() const = 0;

            virtual bool isEOF() const = 0;

            /**
             * Will only be called with other from same index as this.
             * All EOF locs should be considered equal.
             */
             virtual bool pointsToSamePlaceAs(const Cursor& other) const = 0;

            /**
             * If the BtreeInterface impl calls the BucketNotificationCallback, the argument must
             * be forwarded to all Cursors over that Btree.
             * TODO something better.
             */
            virtual void aboutToDeleteBucket(const DiskLoc& bucket) = 0;

            virtual bool locate(const BSONObj& key, const DiskLoc& loc) = 0;

            virtual void advanceTo(const BSONObj &keyBegin,
                                   int keyBeginLen,
                                   bool afterKey,
                                   const vector<const BSONElement*>& keyEnd,
                                   const vector<bool>& keyEndInclusive) = 0;

            /**
             * Locate a key with fields comprised of a combination of keyBegin fields and keyEnd
             * fields.
             */
            virtual void customLocate(const BSONObj& keyBegin,
                                      int keyBeginLen,
                                      bool afterVersion,
                                      const vector<const BSONElement*>& keyEnd,
                                      const vector<bool>& keyEndInclusive) = 0;

            /**
             * Return OK if it's not
             * Otherwise return a status that can be displayed 
             */
            virtual BSONObj getKey() const = 0;

            virtual DiskLoc getDiskLoc() const = 0;

            virtual void advance() = 0;

            //
            // Saving and restoring state
            //
            virtual void savePosition() = 0;

            virtual void restorePosition() = 0;
        };

        /**
         * Caller takes ownership. BtreeInterface must outlive all Cursors it produces.
         */
        virtual Cursor* newCursor(OperationContext* txn, int direction) const = 0;

        //
        // Index creation
        //

        virtual Status initAsEmpty(OperationContext* txn) = 0;
    };

}  // namespace mongo
