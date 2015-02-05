// record_store.h

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

#include <boost/optional.hpp>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"

namespace mongo {

    class CappedDocumentDeleteCallback;
    class Collection;
    struct CompactOptions;
    struct CompactStats;
    class DocWriter;
    class MAdvise;
    class NamespaceDetails;
    class OperationContext;
    class Record;
    class RecordFetcher;

    class RecordStoreCompactAdaptor;
    class RecordStore;

    struct ValidateResults;
    class ValidateAdaptor;

    /**
     * Allows inserting a Record "in-place" without creating a copy ahead of time.
     */
    class DocWriter {
    public:
        virtual ~DocWriter() {}
        virtual void writeDocument( char* buf ) const = 0;
        virtual size_t documentSize() const = 0;
        virtual bool addPadding() const { return true; }
    };

    /**
     * @see RecordStore::updateRecord
     */
    class UpdateNotifier {
    public:
        virtual ~UpdateNotifier(){}
        virtual Status recordStoreGoingToMove( OperationContext* txn,
                                               const RecordId& oldLocation,
                                               const char* oldBuffer,
                                               size_t oldSize ) = 0;
        virtual Status recordStoreGoingToUpdateInPlace( OperationContext* txn,
                                                        const RecordId& loc ) = 0;
    };

    /**
     * A RecordIterator provides an interface for walking over a RecordStore.
     * The details of navigating the collection's structure are below this interface.
     */
    class RecordIterator {
    public:
        virtual ~RecordIterator() { }

        // True if getNext will produce no more data, false otherwise.
        virtual bool isEOF() = 0;

        // Return the RecordId that the iterator points at.  Returns RecordId() if isEOF.
        virtual RecordId curr() = 0;

        // Return the RecordId that the iterator points at and move the iterator to the next item
        // from the collection.  Returns RecordId() if isEOF.
        virtual RecordId getNext() = 0;

        // Can only be called after saveState and before restoreState.
        virtual void invalidate(const RecordId& dl) = 0;

        // Save any state required to resume operation (without crashing) after RecordId deletion or
        // a collection drop.
        virtual void saveState() = 0;

        // Returns true if collection still exists, false otherwise.
        // The state of the iterator may be restored into a different context
        // than the one it was created in.
        virtual bool restoreState(OperationContext* txn) = 0;

        // normally this will just go back to the RecordStore and convert
        // but this gives the iterator an oppurtnity to optimize
        virtual RecordData dataFor( const RecordId& loc ) const = 0;
    };


    class RecordStore {
        MONGO_DISALLOW_COPYING(RecordStore);
    public:
        RecordStore( const StringData& ns ) : _ns(ns.toString()) { }

        virtual ~RecordStore() { }

        // META

        // name of the RecordStore implementation
        virtual const char* name() const = 0;

        virtual const std::string& ns() const { return _ns; }

        virtual long long dataSize( OperationContext* txn ) const = 0;

        virtual long long numRecords( OperationContext* txn ) const = 0;

        virtual bool isCapped() const = 0;

        virtual void setCappedDeleteCallback(CappedDocumentDeleteCallback*) {invariant( false );}

        /**
         * @param extraInfo - optional more debug info
         * @param level - optional, level of debug info to put in (higher is more)
         */
        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0 ) const = 0;

        // CRUD related

        virtual RecordData dataFor( OperationContext* txn, const RecordId& loc) const = 0;

        /**
         * @param out - If the record exists, the contents of this are set.
         * @return true iff there is a Record for loc
         */
        virtual bool findRecord( OperationContext* txn,
                                 const RecordId& loc,
                                 RecordData* out ) const = 0;

        virtual void deleteRecord( OperationContext* txn, const RecordId& dl ) = 0;

        virtual StatusWith<RecordId> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota ) = 0;

        virtual StatusWith<RecordId> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota ) = 0;

        /**
         * @param notifier - Only used by record stores which do not support doc-locking.
         *                   In the case of a document move, this is called after the document
         *                   has been written to the new location, but before it is deleted from
         *                   the old location.
         *                   In the case of an in-place update, this is called just before the
         *                   in-place write occurs.
         * @return Status or RecordId, RecordId might be different
         */
        virtual StatusWith<RecordId> updateRecord( OperationContext* txn,
                                                  const RecordId& oldLocation,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota,
                                                  UpdateNotifier* notifier ) = 0;

        /**
         * @return Returns 'false' if this record store does not implement
         * 'updatewithDamages'. If this method returns false, 'updateWithDamages' must not be
         * called, and all updates must be routed through 'updateRecord' above. This allows the
         * update framework to avoid doing the work of damage tracking if the underlying record
         * store cannot utilize that information.
         */
        virtual bool updateWithDamagesSupported() const = 0;

        virtual Status updateWithDamages( OperationContext* txn,
                                          const RecordId& loc,
                                          const RecordData& oldRec,
                                          const char* damageSource,
                                          const mutablebson::DamageVector& damages ) = 0;

        /**
         * Storage engines which do not support document-level locking hold locks at
         * collection or database granularity. As an optimization, these locks can be yielded
         * when a record needs to be fetched from secondary storage. If this method returns
         * non-NULL, then it indicates that the query system layer should yield and reacquire its
         * locks.
         *
         * The return value is a functor that should be invoked when the locks are yielded;
         * it should access the record at 'loc' so that a potential page fault is triggered
         * out of the lock.
         *
         * The caller is responsible for deleting the return value.
         *
         * Storage engines which support document-level locking need not implement this.
         */
        virtual RecordFetcher* recordNeedsFetch( OperationContext* txn,
                                                 const RecordId& loc ) const { return NULL; }

        /**
         * returned iterator owned by caller
         * Default arguments return all items in record store.
         */
        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const RecordId& start = RecordId(),
                                             const CollectionScanParams::Direction& dir =
                                                     CollectionScanParams::FORWARD
                                             ) const = 0;

        /**
         * Constructs an iterator over a potentially corrupted store, which can be used to salvage
         * damaged records. The iterator might return every record in the store if all of them 
         * are reachable and not corrupted.  Returns NULL if not supported.
         */
        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const {
            return NULL;
        }

        /**
         * Returns many iterators that partition the RecordStore into many disjoint sets. Iterating
         * all returned iterators is equivalent to Iterating the full store.
         */
        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const = 0;

        // higher level


        /**
         * removes all Records
         */
        virtual Status truncate( OperationContext* txn ) = 0;

        /**
         * Truncate documents newer than the document at 'end' from the capped
         * collection.  The collection cannot be completely emptied using this
         * function.  An assertion will be thrown if that is attempted.
         * @param inclusive - Truncate 'end' as well iff true
         * XXX: this will go away soon, just needed to move for now
         */
        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              RecordId end,
                                              bool inclusive) = 0;

        /**
         * does this RecordStore support the compact operation?
         *
         * If you return true, you must provide implementations of all compact methods.
         */
        virtual bool compactSupported() const { return false; }

        /**
         * Does compact() leave RecordIds alone or can they change.
         *
         * Only called if compactSupported() returns true.
         */
        virtual bool compactsInPlace() const { invariant(false); }

        /**
         * Attempt to reduce the storage space used by this RecordStore.
         *
         * Only called if compactSupported() returns true.
         * No RecordStoreCompactAdaptor will be passed if compactsInPlace() returns true.
         */
        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats ) {
            invariant(false);
        }

        /**
         * @param full - does more checks
         * @param scanData - scans each document
         * @return OK if the validate run successfully
         *         OK will be returned even if corruption is found
         *         deatils will be in result
         */
        virtual Status validate( OperationContext* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) = 0;

        /**
         * @param scaleSize - amount by which to scale size metrics
         * appends any custom stats from the RecordStore or other unique stats
         */
        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const = 0;

        /**
         * Load all data into cache.
         * What cache depends on implementation.
         *
         * If the underlying storage engine does not support the operation,
         * returns ErrorCodes::CommandNotSupported
         *
         * @param output (optional) - where to put detailed stats
         */
        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const {
            return Status(ErrorCodes::CommandNotSupported,
                          "this storage engine does not support touch");
        }

        /**
         * @return Status::OK() if option hanlded
         *         InvalidOptions is option not supported
         *         other errors indicate option supported, but error setting
         */
        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL ) = 0;

        /**
         * Return the RecordId of an oplog entry as close to startingPosition as possible without
         * being higher. If there are no entries <= startingPosition, return RecordId().
         *
         * If you don't implement the oplogStartHack, just use the default implementation which
         * returns boost::none.
         */
        virtual boost::optional<RecordId> oplogStartHack(OperationContext* txn,
                                                         const RecordId& startingPosition) const {
            return boost::none;
        }

        /**
         * When we write to an oplog, we call this so that if the storage engine
         * supports doc locking, it can manage the visibility of oplog entries to ensure
         * they are ordered.
         */
        virtual Status oplogDiskLocRegister( OperationContext* txn,
                                             const OpTime& opTime ) {
            return Status::OK();
        }

        /**
         * Called after a repair operation is run with the recomputed numRecords and dataSize.
         */
        virtual void updateStatsAfterRepair(OperationContext* txn,
                                            long long numRecords,
                                            long long dataSize) = 0;

    protected:
        std::string _ns;
    };

    class RecordStoreCompactAdaptor {
    public:
        virtual ~RecordStoreCompactAdaptor(){}
        virtual bool isDataValid( const RecordData& recData ) = 0;
        virtual size_t dataSize( const RecordData& recData ) = 0;
        virtual void inserted( const RecordData& recData, const RecordId& newLocation ) = 0;
    };

    struct ValidateResults {
        ValidateResults() {
            valid = true;
        }
        bool valid;
        std::vector<std::string> errors;
    };

    /**
     * This is so when a RecordStore is validating all records
     * it can call back to someone to check if a record is valid.
     * The actual data contained in a Record is totally opaque to the implementation.
     */
    class ValidateAdaptor {
    public:
        virtual ~ValidateAdaptor(){}

        virtual Status validate( const RecordData& recordData, size_t* dataSize ) = 0;
    };
}
