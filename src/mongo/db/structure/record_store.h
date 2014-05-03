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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/exec/collection_scan_common.h"

namespace mongo {

    class Collection;
    struct CompactOptions;
    struct CompactStats;
    class DeletedRecord;
    class DocWriter;
    class ExtentManager;
    class MAdvise;
    class NamespaceDetails;
    class Record;
    class TransactionExperiment;

    class RecordStoreCompactAdaptor;
    class RecordStore;

    struct ValidateResults;
    class ValidateAdaptor;

    /**
     * A RecordIterator provides an interface for walking over a RecordStore.
     * The details of navigating the collection's structure are below this interface.
     */
    class RecordIterator {
    public:
        virtual ~RecordIterator() { }

        // True if getNext will produce no more data, false otherwise.
        virtual bool isEOF() = 0;

        // Return the DiskLoc that the iterator points at.  Returns DiskLoc() if isEOF.
        virtual DiskLoc curr() = 0;

        // Return the DiskLoc that the iterator points at and move the iterator to the next item
        // from the collection.  Returns DiskLoc() if isEOF.
        virtual DiskLoc getNext() = 0;

        // Can only be called after prepareToYield and before recoverFromYield.
        virtual void invalidate(const DiskLoc& dl) = 0;

        // Save any state required to resume operation (without crashing) after DiskLoc deletion or
        // a collection drop.
        virtual void prepareToYield() = 0;

        // Returns true if collection still exists, false otherwise.
        virtual bool recoverFromYield() = 0;

        // normally this will just go back to the RecordStore and convert
        // but this gives the iterator an oppurtnity to optimize
        virtual const Record* recordFor( const DiskLoc& loc ) const = 0;
    };


    class RecordStore {
        MONGO_DISALLOW_COPYING(RecordStore);
    public:
        RecordStore( const StringData& ns );
        virtual ~RecordStore();

        // META

        // name of the RecordStore implementation
        virtual const char* name() const = 0;

        // CRUD related

        virtual Record* recordFor( const DiskLoc& loc ) const = 0;

        virtual void deleteRecord( TransactionExperiment* txn, const DiskLoc& dl ) = 0;

        virtual StatusWith<DiskLoc> insertRecord( TransactionExperiment* txn,
                                                  const char* data,
                                                  int len,
                                                  int quotaMax ) = 0;

        virtual StatusWith<DiskLoc> insertRecord( TransactionExperiment* txn,
                                                  const DocWriter* doc,
                                                  int quotaMax ) = 0;

        /**
         * returned iterator owned by caller
         * canonical to get all would be
         * getIterator( DiskLoc(), false, CollectionScanParams::FORWARD )
         */
        virtual RecordIterator* getIterator( const DiskLoc& start, bool tailable,
                                             const CollectionScanParams::Direction& dir) const = 0;

        /**
         * Constructs an iterator over a potentially corrupted store, which can be used to salvage
         * damaged records. The iterator might return every record in the store if all of them 
         * are reachable and not corrupted.
         */
        virtual RecordIterator* getIteratorForRepair() const = 0;

        /**
         * Returns many iterators that partition the RecordStore into many disjoint sets. Iterating
         * all returned iterators is equivalent to Iterating the full store.
         */
        virtual std::vector<RecordIterator*> getManyIterators() const = 0;

        // higher level


        /**
         * removes all Records
         */
        virtual Status truncate( TransactionExperiment* txn ) = 0;

        // does this RecordStore support the compact operation
        virtual bool compactSupported() const = 0;
        virtual Status compact( TransactionExperiment* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats ) = 0;

        /**
         * @param full - does more checks
         * @param scanData - scans each document
         * @return OK if the validate run successfully
         *         OK will be returned even if corruption is found
         *         deatils will be in result
         */
        virtual Status validate( TransactionExperiment* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const = 0;

        /**
         * Load all data into cache.
         * What cache depends on implementation.
         * @param output (optional) - where to put detailed stats
         */
        virtual Status touch( TransactionExperiment* txn, BSONObjBuilder* output ) const = 0;

        // TODO: this makes me sad, it shouldn't be in the interface
        // do not use this anymore
        virtual void increaseStorageSize( TransactionExperiment* txn,  int size, int quotaMax ) = 0;

        // TODO: another sad one
        virtual const DeletedRecord* deletedRecordFor( const DiskLoc& loc ) const = 0;

    protected:
        std::string _ns;
    };

    class RecordStoreCompactAdaptor {
    public:
        virtual ~RecordStoreCompactAdaptor(){}
        virtual bool isDataValid( Record* rec ) = 0;
        virtual size_t dataSize( Record* rec ) = 0;
        virtual void inserted( Record* rec, const DiskLoc& newLocation ) = 0;
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

        virtual Status validate( Record* record, size_t* dataSize ) = 0;
    };
}
