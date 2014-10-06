// sorted_data_interface_test_harness.cpp

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

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST( SortedDataInterface, InsertWithDups1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 2 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 6, 2 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );

            long long x = 0;
            sorted->fullValidate( opCtx.get(), &x );
            ASSERT_EQUALS( 2, x );
        }
    }

    TEST( SortedDataInterface, InsertWithDups2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 20 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }
    }

    TEST( SortedDataInterface, InsertWithDups3AndRollback ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 20 ), true );
                // no commit
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }
    }

    TEST( SortedDataInterface, InsertNoDups1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ), false );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 2 ), DiskLoc( 5, 20 ), false );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

    }

    TEST( SortedDataInterface, InsertNoDups2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 2 ), false );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 4 ), false );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

    }

    TEST( SortedDataInterface, Unindex1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT( !sorted->unindex( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 20 ) ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT( !sorted->unindex( opCtx.get(), BSON( "" << 2 ), DiskLoc( 5, 18 ) ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }


        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT( sorted->unindex( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ) ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

    }

    TEST( SortedDataInterface, Unindex2Rollback ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT( sorted->unindex( opCtx.get(), BSON( "" << 1 ), DiskLoc( 5, 18 ) ) );
                // no commit
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

    }


    TEST( SortedDataInterface, CursorIterate1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        int N = 5;
        for ( int i = 0; i < N; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << i ), DiskLoc( 5, i * 2 ), true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            cursor->locate( BSONObj(), minDiskLoc );
            int n = 0;
            while ( !cursor->isEOF() ) {
                DiskLoc loc = cursor->getDiskLoc();
                ASSERT_EQUALS( n * 2, loc.getOfs() );
                ASSERT_EQUALS( BSON( "" << n ), cursor->getKey() );
                n++;
                cursor->advance();
            }
            ASSERT_EQUALS( N, n );
        }


    }

    TEST( SortedDataInterface, CursorIterate1WithSaveRestore ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        int N = 5;
        for ( int i = 0; i < N; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << i ), DiskLoc( 5, i * 2 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            cursor->locate( BSONObj(), minDiskLoc );
            int n = 0;
            while ( !cursor->isEOF() ) {
                DiskLoc loc = cursor->getDiskLoc();
                ASSERT_EQUALS( n * 2, loc.getOfs() );
                ASSERT_EQUALS( BSON( "" << n ), cursor->getKey() );
                n++;
                cursor->advance();
                cursor->savePosition();
                cursor->restorePosition( opCtx.get() );
            }
            ASSERT_EQUALS( N, n );
        }

    }


    TEST( SortedDataInterface, CursorIterate2WithSaveRestore ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        int N = 5;
        for ( int i = 0; i < N; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                sorted->insert( opCtx.get(), BSON( "" << 5 ), DiskLoc( 5, i * 2 ), true );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            cursor->locate( BSONObj(), minDiskLoc );
            int n = 0;
            while ( !cursor->isEOF() ) {
                DiskLoc loc = cursor->getDiskLoc();
                ASSERT_EQUALS( n * 2, loc.getOfs() );
                n++;
                cursor->advance();
                cursor->savePosition();
                cursor->restorePosition( opCtx.get() );
            }
            ASSERT_EQUALS( N, n );
        }

    }


    TEST( SortedDataInterface, Locate1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        BSONObj key = BSON( "" << 1 );
        DiskLoc loc( 5, 16 );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( key, loc ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                Status res = sorted->insert( opCtx.get(), key, loc, true );
                ASSERT_OK( res );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( cursor->locate( key, loc ) );
            ASSERT_EQUALS( key, cursor->getKey() );
            ASSERT_EQUALS( loc, cursor->getDiskLoc() );
        }
    }

    TEST( SortedDataInterface, Locate2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc(1,2), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 2 ), DiskLoc(1,4), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 3 ), DiskLoc(1,6), true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
            ASSERT( !cursor->isEOF()  );
            ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
            ASSERT_EQUALS( DiskLoc(1,4), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
            ASSERT_EQUALS( DiskLoc(1,6), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    TEST( SortedDataInterface, Locate2Empty ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc(1,2), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 2 ), DiskLoc(1,4), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 3 ), DiskLoc(1,6), true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( BSONObj(), DiskLoc(0,0) ) );
            ASSERT( !cursor->isEOF()  );
            ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
            ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( BSONObj(), DiskLoc(0,0) ) );
            ASSERT( cursor->isEOF()  );
        }

    }


    TEST( SortedDataInterface, Locate3Descending ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            for ( int i = 0; i < 10; i++ ) {
                if ( i == 6 )
                    continue;
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << i ), DiskLoc(1,i*2), true ) );
                uow.commit();
            }
        }

        scoped_ptr<OperationContext> opCtx(harnessHelper->newOperationContext());
        scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
        ASSERT( !cursor->locate( BSON( "" << 5 ), DiskLoc(0,0) ) );
        ASSERT( !cursor->isEOF()  );
        ASSERT_EQUALS( BSON( "" << 5 ), cursor->getKey() );
        cursor->advance();
        ASSERT_EQUALS( BSON( "" << 7 ), cursor->getKey() );

        cursor.reset( sorted->newCursor( opCtx.get(), -1 ) );
        ASSERT( !cursor->locate( BSON( "" << 5 ), DiskLoc(0,0) ) );
        ASSERT( !cursor->isEOF()  );
        ASSERT_EQUALS( BSON( "" << 4 ), cursor->getKey() );

        cursor.reset( sorted->newCursor( opCtx.get(), -1 ) );
        ASSERT( !cursor->locate( BSON( "" << 5 ), maxDiskLoc ) );
        ASSERT( !cursor->isEOF()  );
        ASSERT_EQUALS( BSON( "" << 5 ), cursor->getKey() );
        cursor->advance();
        ASSERT_EQUALS( BSON( "" << 4 ), cursor->getKey() );

        cursor.reset( sorted->newCursor( opCtx.get(), -1 ) );
        ASSERT( !cursor->locate( BSON( "" << 5 ), minDiskLoc ) );
        ASSERT( !cursor->isEOF()  );
        ASSERT_EQUALS( BSON( "" << 4 ), cursor->getKey() );
        cursor->advance();
        ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );

        cursor.reset( sorted->newCursor( opCtx.get(), -1 ) );
        cursor->locate( BSON( "" << 6 ), maxDiskLoc );
        ASSERT( !cursor->isEOF()  );
        ASSERT_EQUALS( BSON( "" << 5 ), cursor->getKey() );
        cursor->advance();
        ASSERT_EQUALS( BSON( "" << 4 ), cursor->getKey() );

        cursor.reset( sorted->newCursor( opCtx.get(), -1 ) );
        cursor->locate( BSON( "" << 500 ), maxDiskLoc );
        ASSERT( !cursor->isEOF()  );
        ASSERT_EQUALS( BSON( "" << 9 ), cursor->getKey() );
        cursor->advance();
        ASSERT_EQUALS( BSON( "" << 8 ), cursor->getKey() );

    }

    TEST( SortedDataInterface, Locate4 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc(1,2), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc(1,4), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 1 ), DiskLoc(1,6), true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), BSON( "" << 2 ), DiskLoc(1,8), true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( BSON( "a" << 1 ), minDiskLoc ) );
            ASSERT( !cursor->isEOF()  );
            ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( DiskLoc(1,4), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( DiskLoc(1,6), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( DiskLoc(1,8), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( BSON( "a" << 1 ), maxDiskLoc ) );
            ASSERT( !cursor->isEOF()  );
            ASSERT( cursor->getDirection() == -1 );
            ASSERT_EQUALS( DiskLoc(1,6), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( DiskLoc(1,4), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

} // namespace mongo
