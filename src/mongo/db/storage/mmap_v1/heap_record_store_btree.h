// heap_record_store_btree.h

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

#pragma once

#include <boost/shared_array.hpp>
#include <map>

#include "mongo/db/storage/record_store.h"

namespace mongo {

    /**
     * A RecordStore that stores all data on the heap. This implementation contains only the
     * functionality necessary to test btree.
     */
    class HeapRecordStoreBtree : public RecordStore {
        struct Record;

    public:
        // DiskLoc(0,0) isn't valid for records.
        explicit HeapRecordStoreBtree(const StringData& ns): RecordStore(ns), _nextId(1) { }

        virtual RecordData dataFor(const DiskLoc& loc) const;

        virtual void deleteRecord(OperationContext* txn, const DiskLoc& dl);

        virtual StatusWith<DiskLoc> insertRecord(OperationContext* txn,
                                                 const char* data,
                                                 int len,
                                                 bool enforceQuota);

        virtual StatusWith<DiskLoc> insertRecord(OperationContext* txn,
                                                 const DocWriter* doc,
                                                 bool enforceQuota);

        virtual long long numRecords() const { return _records.size(); }

        virtual Status touch(OperationContext* txn, BSONObjBuilder* output) const;

        typedef std::map<DiskLoc, HeapRecordStoreBtree::Record> Records;

        // public methods below here are not necessary to test btree, and will crash when called.

        // ------------------------------

        virtual StatusWith<DiskLoc> updateRecord(OperationContext* txn,
                                                 const DiskLoc& oldLocation,
                                                 const char* data,
                                                 int len,
                                                 bool enforceQuota,
                                                 UpdateMoveNotifier* notifier) {
            invariant(false);
        }

        virtual Status updateWithDamages(OperationContext* txn,
                                         const DiskLoc& loc,
                                         const char* damangeSource,
                                         const mutablebson::DamageVector& damages) {
            invariant(false);
        }

        virtual RecordIterator* getIterator(OperationContext* txn,
                                            const DiskLoc& start,
                                            bool tailable,
                                            const CollectionScanParams::Direction& dir) const {
            invariant(false);
        }

        virtual RecordIterator* getIteratorForRepair(OperationContext* txn) const {
            invariant(false);
        }

        virtual std::vector<RecordIterator*> getManyIterators(OperationContext* txn) const {
            invariant(false);
        }

        virtual Status truncate(OperationContext* txn) { invariant(false); }

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive) {
            invariant(false);
        }

        virtual bool compactSupported() const { invariant(false); }

        virtual Status compact(OperationContext* txn,
                               RecordStoreCompactAdaptor* adaptor,
                               const CompactOptions* options,
                               CompactStats* stats) {
            invariant(false);
        }

        virtual Status validate(OperationContext* txn,
                                bool full,
                                bool scanData,
                                ValidateAdaptor* adaptor,
                                ValidateResults* results, BSONObjBuilder* output) const {
            invariant(false);
        }

        virtual void appendCustomStats(OperationContext* txn,
                                       BSONObjBuilder* result,
                                       double scale) const {
            invariant(false);
        }

        virtual Status setCustomOption(OperationContext* txn,
                                       const BSONElement& option,
                                       BSONObjBuilder* info = NULL) {
            invariant(false);
        }

        virtual void increaseStorageSize(OperationContext* txn,  int size, bool enforceQuota) {
            invariant(false);
        }

        virtual int64_t storageSize(OperationContext* txn,
                                    BSONObjBuilder* extraInfo = NULL,
                                    int infoLevel = 0) const {
            invariant(false);
        }

        virtual long long dataSize() const { invariant(false); }

        virtual Record* recordFor(const DiskLoc& loc) const { invariant(false); }

        virtual bool isCapped() const { invariant(false); }

        virtual const char* name() const { invariant(false); }
        // more things that we actually care about below

    private:
        struct Record {
            Record(): dataSize(-1), data() { }
            explicit Record(int size): dataSize(size), data(new char[size]) { }

            int dataSize;
            boost::shared_array<char> data;
        };

        DiskLoc allocateLoc();

        Records _records;
        int64_t _nextId;
    };

} // namespace mongo
