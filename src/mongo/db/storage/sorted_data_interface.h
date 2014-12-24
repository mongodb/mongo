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
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"

#pragma once

namespace mongo {

    class BSONObjBuilder;
    class BucketDeletionNotification;
    class SortedDataBuilderInterface;

    /**
     * This interface is a work in progress.  Notes below:
     *
     * This interface began as the SortedDataInterface, a way to hide the fact that there were two
     * on-disk formats for the btree.  With the introduction of other storage engines, this
     * interface was generalized to provide access to sorted data.  Specifically:
     *
     * 1. Many other storage engines provide different Btree(-ish) implementations.  This interface
     * could allow those interfaces to avoid storing btree buckets in an already sorted structure.
     *
     * TODO: See if there is actually a performance gain.
     *
     * 2. The existing btree implementation is written to assume that if it modifies a record it is
     * modifying the underlying record.  This interface is an attempt to work around that.
     *
     * TODO: See if this actually works.
     */
    class SortedDataInterface {
    public:
        virtual ~SortedDataInterface() { }

        //
        // Data changes
        //

        /**
         * Return a bulk builder for 'this' index.
         *
         * Implementations can assume that 'this' index outlives its bulk
         * builder.
         *
         * @param txn the transaction under which keys are added to 'this' index
         * @param dupsAllowed true if duplicate keys are allowed, and false
         *        otherwise
         *
         * @return caller takes ownership
         */
        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn,
                                                           bool dupsAllowed) = 0;

        /**
         * Insert an entry into the index with the specified key and RecordId.
         *
         * @param txn the transaction under which the insert takes place
         * @param dupsAllowed true if duplicate keys are allowed, and false
         *        otherwise
         *
         * @return Status::OK() if the insert succeeded,
         *
         *         ErrorCodes::DuplicateKey if 'key' already exists in 'this' index
         *         at a RecordId other than 'loc' and duplicates were not allowed
         */
        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const RecordId& loc,
                              bool dupsAllowed) = 0;

        /**
         * Remove the entry from the index with the specified key and RecordId.
         *
         * @param txn the transaction under which the remove takes place
         * @param dupsAllowed true if duplicate keys are allowed, and false
         *        otherwise
         */
        virtual void unindex(OperationContext* txn,
                             const BSONObj& key,
                             const RecordId& loc,
                             bool dupsAllowed) = 0;

        /**
         * Return ErrorCodes::DuplicateKey if 'key' already exists in 'this'
         * index at a RecordId other than 'loc', and Status::OK() otherwise.
         *
         * @param txn the transaction under which this operation takes place
         *
         * TODO: Hide this by exposing an update method?
         */
        virtual Status dupKeyCheck(OperationContext* txn,
                                   const BSONObj& key,
                                   const RecordId& loc) = 0;

        //
        // Information about the tree
        //

        /**
         * 'output' is used to store results of validate when 'full' is true.
         * If 'full' is false, 'output' may be NULL.
         *
         * TODO: expose full set of args for testing?
         */
        virtual void fullValidate(OperationContext* txn, bool full, long long* numKeysOut,
                                  BSONObjBuilder* output) const = 0;

        virtual bool appendCustomStats(OperationContext* txn, BSONObjBuilder* output, double scale)
            const = 0;


        /**
         * Return the number of bytes consumed by 'this' index.
         *
         * @param txn the transaction under which this operation takes place
         *
         * @see IndexAccessMethod::getSpaceUsedBytes
         */
        virtual long long getSpaceUsedBytes( OperationContext* txn ) const = 0;

        /**
         * Return true if 'this' index is empty, and false otherwise.
         */
        virtual bool isEmpty(OperationContext* txn) = 0;

        /**
         * Attempt to bring the entirety of 'this' index into memory.
         *
         * If the underlying storage engine does not support the operation,
         * returns ErrorCodes::CommandNotSupported
         *
         * @return Status::OK()
         */
        virtual Status touch(OperationContext* txn) const {
            return Status(ErrorCodes::CommandNotSupported,
                          "this storage engine does not support touch");
        }

        /**
         * Return the number of entries in 'this' index.
         *
         * The default implementation should be overridden with a more
         * efficient one if at all possible.
         */
        virtual long long numEntries( OperationContext* txn ) const {
            long long x = -1;
            fullValidate(txn, false, &x, NULL);
            return x;
        }

        /**
         * Navigation
         *
         * A cursor is tied to a transaction, such as the OperationContext or a WriteUnitOfWork
         * inside that context. Any cursor acquired inside a transaction is invalid outside
         * of that transaction, instead use the savePosition() and restorePosition() methods
         * reestablish the cursor.
         */
        class Cursor {
        public:
            virtual ~Cursor() {}

            /**
             * Return the direction of 'this' cursor.
             *
             * @return +1 for a forward cursor or -1 for a reverse cursor
             */
            virtual int getDirection() const = 0;

            /**
             * Return true if 'this' forward (reverse) cursor is positioned
             * past the end (before the beginning) of the index, and false otherwise.
             */
            virtual bool isEOF() const = 0;

            /**
             * Return true if 'this' cursor and the 'other' cursor are positioned at
             * the same key and RecordId, or if both cursors are at EOF. Otherwise,
             * this function returns false.
             *
             * Implementations should prohibit the comparison of cursors associated
             * with different indices.
             */
             virtual bool pointsToSamePlaceAs(const Cursor& other) const = 0;

            /**
             * Position 'this' forward (reverse) cursor either at the entry or
             * immediately after (or immediately before) the specified key and RecordId.
             * The cursor should be positioned at EOF if no such entry exists.
             *
             * @return true if the entry (key, RecordId) exists within the index,
             *         and false otherwise
             */
            virtual bool locate(const BSONObj& key, const RecordId& loc) = 0;

            /**
             * Position 'this' forward (reverse) cursor either at the next
             * (previous) occurrence of a particular key or immediately after
             * (or immediately before).
             *
             * @see SortedDataInterface::customLocate
             */
            virtual void advanceTo(const BSONObj &keyPrefix,
                                   int prefixLen,
                                   bool prefixExclusive,
                                   const vector<const BSONElement*>& keySuffix,
                                   const vector<bool>& suffixInclusive) = 0;

            /**
             * Position 'this' forward (reverse) cursor either at the first
             * (last) occurrence of a particular key or immediately after
             * (or immediately before). The key is a typical BSONObj,
             * represented by the specified parameters in the following way:
             *
             * The first 'prefixLen' elements of 'keyPrefix' followed by
             * the last 'keySuffix.size() - prefixLen' elements of 'keySuffix'.
             *
             * e.g.
             *
             *  Suppose that
             *
             *      keyPrefix = { "" : 1, "" : 2 }
             *      prefixLen = 1
             *      prefixExclusive = false
             *      keySuffix = [ IGNORED; { "" : 5 } ]
             *      suffixInclusive = [ IGNORED; false ]
             *
             *      ==> represented key is { "" : 1, "" : 5 }
             *          with the exclusive byte set on the second field
             *
             *  Suppose that
             *
             *      keyPrefix = { "" : 1, "" : 2 }
             *      prefixLen = 1
             *      prefixExclusive = true
             *      keySuffix = IGNORED
             *      suffixInclusive = IGNORED
             *
             *      ==> represented key is { "" : 1 }
             *          with the exclusive byte set on the first field
             *
             * @param prefixExclusive true if 'this' forward (reverse) cursor
             *        should be positioned immediately after (immediately
             *        before) the represented key, and false otherwise
             *
             * @param suffixInclusive an element of the vector is false if
             *        'this' forward (reverse) cursor should be positioned
             *        immediately after (immediately before) the represented
             *        key, and true otherwise
             *
             * Implementations should prohibit callers from specifying
             * 'prefixLen = 0' when 'prefixExclusive = true'.
             *
             * @see IndexEntryComparison::makeQueryObject
             */
            virtual void customLocate(const BSONObj& keyPrefix,
                                      int prefixLen,
                                      bool prefixExclusive,
                                      const vector<const BSONElement*>& keySuffix,
                                      const vector<bool>& suffixInclusive) = 0;

            /**
             * Return the key associated with the current position of 'this' cursor.
             */
            virtual BSONObj getKey() const = 0;

            /**
             * Return the RecordId associated with the current position of 'this' cursor.
             */
            virtual RecordId getRecordId() const = 0;

            /**
             * Position 'this' forward (reverse) cursor at the next (preceding) entry
             * in the index. A cursor positioned at EOF should remain at EOF when advanced.
             */
            virtual void advance() = 0;

            //
            // Saving and restoring state
            //

            /**
             * Save the entry in the index (i.e. its key and RecordId) of where
             * 'this' cursor is currently positioned.
             *
             * Implementations can assume that no operations other than delete
             * or restorePosition() will be called on 'this' cursor after its
             * position has been saved.
             */
            virtual void savePosition() = 0;

            /**
             * Restore 'this' cursor to the previously saved entry in the index.
             *
             * Implementations should have the same behavior as calling locate()
             * with the saved key and RecordId.
             */
            virtual void restorePosition(OperationContext* txn) = 0;
        };

        /**
         * Return a cursor over 'this' index. The cursor need not be positioned
         * at any particular entry.
         *
         * Implementations can assume that locate() is called on the cursor
         * before it gets used.
         *
         * Implementations can assume that 'this' index outlives all cursors
         * it produces.
         *
         * @param txn the transaction to which the cursor is tied
         * @param direction the direction of the cursor.
         *        +1 for a forward cursor or -1 for a reverse cursor.
         *
         * @return caller takes ownership
         */
        virtual Cursor* newCursor(OperationContext* txn, int direction) const = 0;

        //
        // Index creation
        //

        virtual Status initAsEmpty(OperationContext* txn) = 0;
    };

    /**
     * A version-hiding wrapper around the bulk builder for the Btree.
     */
    class SortedDataBuilderInterface {
    public:
        virtual ~SortedDataBuilderInterface() { }

        /**
         * Adds 'key' to intermediate storage.
         *
         * 'key' must be > or >= the last key passed to this function (depends on _dupsAllowed).  If
         * this is violated an error Status (ErrorCodes::InternalError) will be returned.
         */
        virtual Status addKey(const BSONObj& key, const RecordId& loc) = 0;

        /**
         * Do any necessary work to finish building the tree.
         *
         * The default implementation may be used if no commit phase is necessary because addKey
         * always leaves the tree in a valid state.
         *
         * This is called outside of any WriteUnitOfWork to allow implementations to split this up
         * into multiple units.
         */
        virtual void commit(bool mayInterrupt) {}
    };

}  // namespace mongo
