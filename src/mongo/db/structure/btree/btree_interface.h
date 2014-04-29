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
#include "mongo/db/storage/transaction.h"
#include "mongo/db/structure/head_manager.h"
#include "mongo/db/structure/record_store.h"

#pragma once

namespace mongo {

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
     *
     * TODO: do we want to hide the fact that (DiskLoc, int) identify an entry?
     */
    class BtreeInterface {
    public:
        struct SavedPositionData {
            BSONObj key;
            DiskLoc loc;
        };

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
                                            int version);

        //
        // Data changes
        //

        /**
         * Caller owns returned pointer.
         * 'this' must outlive the returned pointer.
         */
        virtual BtreeBuilderInterface* getBulkBuilder(TransactionExperiment* txn,
                                                      bool dupsAllowed) = 0;

        virtual Status insert(TransactionExperiment* txn,
                              const BSONObj& key,
                              const DiskLoc& loc,
                              bool dupsAllowed) = 0;

        virtual bool unindex(TransactionExperiment* txn,
                             const BSONObj& key,
                             const DiskLoc& loc) = 0;

        // TODO: Hide this by exposing an update method?
        virtual Status dupKeyCheck(const BSONObj& key, const DiskLoc& loc) = 0;

        //
        // Information about the tree
        //

        // TODO: expose full set of args for testing?
        virtual void fullValidate(long long* numKeysOut) = 0;

        virtual bool isEmpty() = 0;

        //
        // Navigation
        //

        virtual bool locate(const BSONObj& key,
                            const DiskLoc& loc,
                            const int direction,
                            DiskLoc* bucketOut,
                            int* keyPosOut) = 0;

        virtual void advanceTo(DiskLoc* thisLocInOut,
                               int* keyOfsInOut,
                               const BSONObj &keyBegin,
                               int keyBeginLen,
                               bool afterKey,
                               const vector<const BSONElement*>& keyEnd,
                               const vector<bool>& keyEndInclusive,
                               int direction) const = 0;

        /**
         * Locate a key with fields comprised of a combination of keyBegin fields and keyEnd fields.
         */
        virtual void customLocate(DiskLoc* locInOut,
                                  int* keyOfsInOut,
                                  const BSONObj& keyBegin,
                                  int keyBeginLen,
                                  bool afterVersion,
                                  const vector<const BSONElement*>& keyEnd,
                                  const vector<bool>& keyEndInclusive,
                                  int direction) = 0;

        /**
         * Return OK if it's not
         * Otherwise return a status that can be displayed 
         */
        virtual BSONObj getKey(const DiskLoc& bucket, const int keyOffset) = 0;

        virtual DiskLoc getDiskLoc(const DiskLoc& bucket, const int keyOffset) = 0;

        virtual void advance(DiskLoc* bucketInOut, int* posInOut, int direction) = 0;

        //
        // Saving and restoring state
        //
        virtual void savePosition(const DiskLoc& bucket,
                                  const int keyOffset,
                                  SavedPositionData* savedOut) = 0;

        virtual void restorePosition(const SavedPositionData& saved,
                                     int direction,
                                     DiskLoc* bucketOut,
                                     int* keyOffsetOut) = 0;

        //
        // Index creation
        //

        virtual Status initAsEmpty(TransactionExperiment* txn) = 0;
    };

}  // namespace mongo
