// record_store_test_recorditer.cpp

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

#include "mongo/db/storage/record_store_test_harness.h"

#include <algorithm>

#include "mongo/bson/util/builder.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

using std::string;
using std::stringstream;

namespace mongo {

    // Insert multiple records and iterate through them in the forward direction.
    // When curr() or getNext() is called on an iterator positioned at EOF,
    // the iterator returns DiskLoc() and stays at EOF.
    TEST( RecordStoreTestHarness, IterateOverMultipleRecords ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        const int nToInsert = 10;
        DiskLoc locs[nToInsert];
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                stringstream ss;
                ss << "record " << i;
                string data = ss.str();

                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                            data.c_str(),
                                                            data.size() + 1,
                                                            false );
                ASSERT_OK( res.getStatus() );
                locs[i] = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, rs->numRecords( opCtx.get() ) );
        }

        std::sort( locs, locs + nToInsert ); // inserted records may not be in DiskLoc order
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            RecordIterator *it = rs->getIterator( opCtx.get(),
                                                  DiskLoc(),
                                                  CollectionScanParams::FORWARD );

            for ( int i = 0; i < nToInsert; i++ ) {
                ASSERT( !it->isEOF() );
                ASSERT_EQUALS( locs[i], it->curr() );
                ASSERT_EQUALS( locs[i], it->getNext() );
            }
            ASSERT( it->isEOF() );

            ASSERT_EQUALS( DiskLoc(), it->curr() );
            ASSERT_EQUALS( DiskLoc(), it->getNext() );
            ASSERT( it->isEOF() );
            ASSERT_EQUALS( DiskLoc(), it->curr() );

            delete it;
        }
    }

    // Insert multiple records and iterate through them in the reverse direction.
    // When curr() or getNext() is called on an iterator positioned at EOF,
    // the iterator returns DiskLoc() and stays at EOF.
    TEST( RecordStoreTestHarness, IterateOverMultipleRecordsReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        const int nToInsert = 10;
        DiskLoc locs[nToInsert];
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                stringstream ss;
                ss << "record " << i;
                string data = ss.str();

                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                            data.c_str(),
                                                            data.size() + 1,
                                                            false );
                ASSERT_OK( res.getStatus() );
                locs[i] = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, rs->numRecords( opCtx.get() ) );
        }

        std::sort( locs, locs + nToInsert ); // inserted records may not be in DiskLoc order
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            RecordIterator *it = rs->getIterator( opCtx.get(),
                                                  DiskLoc(),
                                                  CollectionScanParams::BACKWARD );

            for ( int i = nToInsert - 1; i >= 0; i-- ) {
                ASSERT( !it->isEOF() );
                ASSERT_EQUALS( locs[i], it->curr() );
                ASSERT_EQUALS( locs[i], it->getNext() );
            }
            ASSERT( it->isEOF() );

            ASSERT_EQUALS( DiskLoc(), it->curr() );
            ASSERT_EQUALS( DiskLoc(), it->getNext() );
            ASSERT( it->isEOF() );
            ASSERT_EQUALS( DiskLoc(), it->curr() );

            delete it;
        }
    }

    // Insert multiple records and try to create a forward iterator
    // starting at an interior position.
    TEST( RecordStoreTestHarness, IterateStartFromMiddle ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        const int nToInsert = 10;
        DiskLoc locs[nToInsert];
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                stringstream ss;
                ss << "record " << i;
                string data = ss.str();

                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                            data.c_str(),
                                                            data.size() + 1,
                                                            false );
                ASSERT_OK( res.getStatus() );
                locs[i] = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, rs->numRecords( opCtx.get() ) );
        }

        std::sort( locs, locs + nToInsert ); // inserted records may not be in DiskLoc order
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            int start = nToInsert / 2;
            RecordIterator *it = rs->getIterator( opCtx.get(),
                                                  locs[start],
                                                  CollectionScanParams::FORWARD );

            for ( int i = start; i < nToInsert; i++ ) {
                ASSERT( !it->isEOF() );
                ASSERT_EQUALS( locs[i], it->curr() );
                ASSERT_EQUALS( locs[i], it->getNext() );
            }
            ASSERT( it->isEOF() );

            ASSERT_EQUALS( DiskLoc(), it->curr() );
            ASSERT_EQUALS( DiskLoc(), it->getNext() );
            ASSERT( it->isEOF() );
            ASSERT_EQUALS( DiskLoc(), it->curr() );

            delete it;
        }
    }

    // Insert multiple records and try to create a reverse iterator
    // starting at an interior position.
    TEST( RecordStoreTestHarness, IterateStartFromMiddleReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        const int nToInsert = 10;
        DiskLoc locs[nToInsert];
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                stringstream ss;
                ss << "record " << i;
                string data = ss.str();

                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                            data.c_str(),
                                                            data.size() + 1,
                                                            false );
                ASSERT_OK( res.getStatus() );
                locs[i] = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, rs->numRecords( opCtx.get() ) );
        }

        std::sort( locs, locs + nToInsert ); // inserted records may not be in DiskLoc order
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            int start = nToInsert / 2;
            RecordIterator *it = rs->getIterator( opCtx.get(),
                                                  locs[start],
                                                  CollectionScanParams::BACKWARD );

            for ( int i = start; i >= 0; i-- ) {
                ASSERT( !it->isEOF() );
                ASSERT_EQUALS( locs[i], it->curr() );
                ASSERT_EQUALS( locs[i], it->getNext() );
            }
            ASSERT( it->isEOF() );

            ASSERT_EQUALS( DiskLoc(), it->curr() );
            ASSERT_EQUALS( DiskLoc(), it->getNext() );
            ASSERT( it->isEOF() );
            ASSERT_EQUALS( DiskLoc(), it->curr() );

            delete it;
        }
    }

    // Insert several records, and iterate to the end. Ensure that the record iterator
    // is EOF. Add an additional record, saving and restoring the iterator state, and check
    // that the iterator remains EOF.
    TEST( RecordStoreTestHarness, RecordIteratorEOF ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        const int nToInsert = 10;
        DiskLoc locs[nToInsert];
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                StringBuilder sb;
                sb << "record " << i;
                string data = sb.str();

                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                            data.c_str(),
                                                            data.size() + 1,
                                                            false );
                ASSERT_OK( res.getStatus() );
                locs[i] = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, rs->numRecords( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            // Get a forward iterator starting at the beginning of the record store.
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get() ) );

            // Iterate, checking EOF along the way.
            for ( int i = 0; i < nToInsert; i++ ) {
                ASSERT( !it->isEOF() );
                DiskLoc nextLoc = it->getNext();
                ASSERT( !nextLoc.isNull() );
            }
            ASSERT( it->isEOF() );
            ASSERT( it->getNext().isNull() );

            // Add a record and ensure we're still EOF.
            it->saveState();

            StringBuilder sb;
            sb << "record " << nToInsert + 1;
            string data = sb.str();

            WriteUnitOfWork uow( opCtx.get() );
            StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                        data.c_str(),
                                                        data.size() + 1,
                                                        false );
            ASSERT_OK( res.getStatus() );
            uow.commit();

            ASSERT( it->restoreState( opCtx.get() ) );

            // Iterator should still be EOF.
            ASSERT( it->isEOF() );
            ASSERT( it->getNext().isNull() );
        }
    }

} // namespace mongo
