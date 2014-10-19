// record_store_v1_capped.h

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
#include "mongo/db/storage/capped_callback.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"

namespace mongo {

    class CappedRecordStoreV1 : public RecordStoreV1Base {
    public:
        CappedRecordStoreV1( OperationContext* txn,
                             CappedDocumentDeleteCallback* collection,
                             const StringData& ns,
                             RecordStoreV1MetaData* details,
                             ExtentManager* em,
                             bool isSystemIndexes );

        virtual ~CappedRecordStoreV1();

        const char* name() const { return "CappedRecordStoreV1"; }

        virtual Status truncate(OperationContext* txn);

        /**
         * Truncate documents newer than the document at 'end' from the capped
         * collection.  The collection cannot be completely emptied using this
         * function.  An assertion will be thrown if that is attempted.
         * @param inclusive - Truncate 'end' as well iff true
         * XXX: this will go away soon, just needed to move for now
         */
        virtual void temp_cappedTruncateAfter( OperationContext* txn, DiskLoc end, bool inclusive );

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const DiskLoc& start,
                                             const CollectionScanParams::Direction& dir) const;

        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const;

        virtual bool compactSupported() const { return false; }

        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats );

        // Start from firstExtent by default.
        DiskLoc firstRecord( OperationContext* txn,
                             const DiskLoc &startExtent = DiskLoc() ) const;
        // Start from lastExtent by default.
        DiskLoc lastRecord( OperationContext* txn,
                            const DiskLoc &startExtent = DiskLoc() ) const;

    protected:

        virtual bool isCapped() const { return true; }
        virtual bool shouldPadInserts() const { return false; }

        virtual void setCappedDeleteCallback( CappedDocumentDeleteCallback* cb ) {
            _deleteCallback = cb;
        }

        virtual StatusWith<DiskLoc> allocRecord( OperationContext* txn,
                                                 int lengthWithHeaders,
                                                 bool enforceQuota );

        virtual void addDeletedRec(OperationContext* txn, const DiskLoc& dloc);

    private:
        // -- start copy from cap.cpp --
        void compact(OperationContext* txn);
        DiskLoc cappedFirstDeletedInCurExtent() const;
        void setFirstDeletedInCurExtent( OperationContext* txn, const DiskLoc& loc );
        void cappedCheckMigrate(OperationContext* txn);
        DiskLoc __capAlloc( OperationContext* txn, int len );
        bool inCapExtent( const DiskLoc &dl ) const;
        DiskLoc cappedListOfAllDeletedRecords() const;
        DiskLoc cappedLastDelRecLastExtent() const;
        void setListOfAllDeletedRecords( OperationContext* txn, const DiskLoc& loc );
        void setLastDelRecLastExtent( OperationContext* txn, const DiskLoc& loc );
        Extent *theCapExtent() const;
        bool nextIsInCapExtent( const DiskLoc &dl ) const;
        void advanceCapExtent( OperationContext* txn, const StringData& ns );
        void cappedTruncateLastDelUpdate(OperationContext* txn);

        /**
         * Truncate documents newer than the document at 'end' from the capped
         * collection.  The collection cannot be completely emptied using this
         * function.  An assertion will be thrown if that is attempted.
         * @param inclusive - Truncate 'end' as well iff true
         */
        void cappedTruncateAfter(OperationContext* txn,
                                 const char* ns,
                                 DiskLoc end,
                                 bool inclusive);

        void _maybeComplain( OperationContext* txn, int len ) const;

        // -- end copy from cap.cpp --

        CappedDocumentDeleteCallback* _deleteCallback;

        OwnedPointerVector<ExtentManager::CacheHint> _extentAdvice;

        friend class CappedRecordStoreV1Iterator;
    };


}
