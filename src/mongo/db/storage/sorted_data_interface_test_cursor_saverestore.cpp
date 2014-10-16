// sorted_data_interface_test_cursor_saverestore.cpp

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

    // Insert multiple keys and try to iterate through all of them
    // using a forward cursor while calling savePosition() and
    // restorePosition() in succession.
    TEST( SortedDataInterface, SaveAndRestorePositionWhileIterateCursor ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        int nToInsert = 10;
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                BSONObj key = BSON( "" << i );
                DiskLoc loc( 42, i * 2 );
                ASSERT_OK( sorted->insert( opCtx.get(), key, loc, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( minKey, minDiskLoc ) );
            for ( int i = 0; i < nToInsert; i++ ) {
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << i ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc( 42, i * 2 ), cursor->getDiskLoc() );
                cursor->advance();
                cursor->savePosition();
                cursor->restorePosition( opCtx.get() );
            }
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple keys and try to iterate through all of them
    // using a reverse cursor while calling savePosition() and
    // restorePosition() in succession.
    TEST( SortedDataInterface, SaveAndRestorePositionWhileIterateCursorReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        int nToInsert = 10;
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                BSONObj key = BSON( "" << i );
                DiskLoc loc( 42, i * 2 );
                ASSERT_OK( sorted->insert( opCtx.get(), key, loc, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( maxKey, maxDiskLoc ) );
            for ( int i = nToInsert - 1; i >= 0; i-- ) {
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << i ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc( 42, i * 2 ), cursor->getDiskLoc() );
                cursor->advance();
                cursor->savePosition();
                cursor->restorePosition( opCtx.get() );
            }
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert the same key multiple times and try to iterate through each
    // occurrence using a forward cursor while calling savePosition() and
    // restorePosition() in succession. Verify that the DiskLoc is saved
    // as part of the current position of the cursor.
    TEST( SortedDataInterface, SaveAndRestorePositionWhileIterateCursorWithDupKeys ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        int nToInsert = 10;
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                DiskLoc loc( 42, i * 2 );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc, true /* allow duplicates */ ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( minKey, minDiskLoc ) );
            for ( int i = 0; i < nToInsert; i++ ) {
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( DiskLoc( 42, i * 2 ), cursor->getDiskLoc() );
                cursor->advance();
                cursor->savePosition();
                cursor->restorePosition( opCtx.get() );
            }
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert the same key multiple times and try to iterate through each
    // occurrence using a reverse cursor while calling savePosition() and
    // restorePosition() in succession. Verify that the DiskLoc is saved
    // as part of the current position of the cursor.
    TEST( SortedDataInterface, SaveAndRestorePositionWhileIterateCursorWithDupKeysReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        int nToInsert = 10;
        for ( int i = 0; i < nToInsert; i++ ) {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                DiskLoc loc( 42, i * 2 );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc, true /* allow duplicates */ ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( nToInsert, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( maxKey, maxDiskLoc ) );
            for ( int i = nToInsert - 1; i >= 0; i-- ) {
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( DiskLoc( 42, i * 2 ), cursor->getDiskLoc() );
                cursor->advance();
                cursor->savePosition();
                cursor->restorePosition( opCtx.get() );
            }
            ASSERT( cursor->isEOF() );
        }
    }

    // Call savePosition() on a forward cursor without ever calling restorePosition().
    // May be useful to run this test under valgrind to verify there are no leaks.
    TEST( SortedDataInterface, SavePositionWithoutRestore ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( true ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            cursor->savePosition();
        }
    }

    // Call savePosition() on a reverse cursor without ever calling restorePosition().
    // May be useful to run this test under valgrind to verify there are no leaks.
    TEST( SortedDataInterface, SavePositionWithoutRestoreReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            cursor->savePosition();
        }
    }

} // namespace mongo
