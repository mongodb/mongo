// kv_record_store_capped.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
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

#include "mongo/db/operation_context.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/kv/dictionary/kv_record_store_capped.h"

namespace mongo {

    KVRecordStoreCapped::KVRecordStoreCapped( KVDictionary *db,
                                              OperationContext* opCtx,
                                              const StringData& ns,
                                              const StringData& ident,
                                              const CollectionOptions& options ) :
        KVRecordStore(db, opCtx, ns, ident, options),
        _cappedMaxSize(options.cappedSize ? options.cappedSize : 4096 ),
        _cappedMaxDocs(options.cappedMaxDocs ? options.cappedMaxDocs : -1),
        _cappedDeleteCallback(NULL) {
    }

    bool KVRecordStoreCapped::needsDelete(OperationContext* txn) const {
        if (dataSize(txn) > _cappedMaxSize) {
            // .. too many bytes
            return true;
        }

        if ((_cappedMaxDocs != -1) && (numRecords(txn) > _cappedMaxDocs)) {
            // .. too many documents
            return true;
        }

        // we're ok
        return false;
    }

    void KVRecordStoreCapped::deleteAsNeeded(OperationContext *txn) {
        if (!needsDelete(txn)) {
            // nothing to do
            return;
        }

        // Delete documents while we are over-full and the iterator has more.
        for (boost::scoped_ptr<RecordIterator> iter(getIterator(txn));
             needsDelete(txn) && !iter->isEOF(); ) {
            const DiskLoc oldest = iter->getNext();
            deleteRecord(txn, oldest);
        }
    }

    StatusWith<DiskLoc> KVRecordStoreCapped::insertRecord( OperationContext* txn,
                                                           const char* data,
                                                           int len,
                                                           bool enforceQuota ) {
        if (len > _cappedMaxSize) {
            // this single document won't fit
            return StatusWith<DiskLoc>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        // insert using the regular KVRecordStore insert implementation..
        const StatusWith<DiskLoc> status =
            KVRecordStore::insertRecord(txn, data, len, enforceQuota);

        // ..then delete old data as needed
        deleteAsNeeded(txn);

        return status;
    }

    StatusWith<DiskLoc> KVRecordStoreCapped::insertRecord( OperationContext* txn,
                                                           const DocWriter* doc,
                                                           bool enforceQuota ) {
        // We need to override every insertRecord overload, otherwise the compiler gets mad.
        Slice value(doc->documentSize());
        doc->writeDocument(value.mutableData());
        return insertRecord(txn, value.data(), value.size(), enforceQuota);
    }

    void KVRecordStoreCapped::deleteRecord( OperationContext* txn, const DiskLoc& dl ) {
        if (_cappedDeleteCallback) {
            // need to notify higher layers that a diskloc is about to be deleted
            uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, dl));
        }
        KVRecordStore::deleteRecord(txn, dl);
    }

    void KVRecordStoreCapped::appendCustomStats( OperationContext* txn,
                                                 BSONObjBuilder* result,
                                                 double scale ) const {
        result->append("capped", true);
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", _cappedMaxSize);
        KVRecordStore::appendCustomStats(txn, result, scale);
    }

    void KVRecordStoreCapped::temp_cappedTruncateAfter(OperationContext* txn,
                                                       DiskLoc end,
                                                       bool inclusive) {
        // Not very efficient, but it should only be used by tests.
        for (boost::scoped_ptr<RecordIterator> iter(
                 getIterator(txn, end, CollectionScanParams::FORWARD));
             !iter->isEOF(); ) {
            DiskLoc loc = iter->getNext();
            if (!inclusive && loc == end) {
                continue;
            }
            WriteUnitOfWork wu( txn );
            deleteRecord(txn, loc);
            wu.commit();
        }
    }

} // namespace mongo
