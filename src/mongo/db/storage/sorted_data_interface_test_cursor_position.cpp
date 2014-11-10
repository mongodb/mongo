// sorted_data_interface_test_cursor_position.cpp

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

    // Verify that two forward cursors positioned at EOF are considered
    // to point to the same place.
    TEST( SortedDataInterface, CursorsPointToSamePlaceIfEOF ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), 1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( !cursor1->locate( minKey, minDiskLoc ) );
            ASSERT( !cursor2->locate( minKey, minDiskLoc ) );
            ASSERT( cursor1->isEOF() );
            ASSERT( cursor2->isEOF() );
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that two reverse cursors positioned at EOF are considered
    // to point to the same place.
    TEST( SortedDataInterface, CursorsPointToSamePlaceIfEOFReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), -1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( !cursor1->locate( maxKey, maxDiskLoc ) );
            ASSERT( !cursor2->locate( maxKey, maxDiskLoc ) );
            ASSERT( cursor1->isEOF() );
            ASSERT( cursor2->isEOF() );
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Iterate two forward cursors simultaneously and verify they are considered
    // to point to the same place.
    TEST( SortedDataInterface, CursorsPointToSamePlace ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc2, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), 1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor1->locate( key1, loc1 ) );
            ASSERT( cursor2->locate( key1, loc1 ) );
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );

            cursor1->advance();
            cursor2->advance();
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );

            cursor1->advance();
            cursor2->advance();
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Iterate two reverse cursors simultaneously and verify they are considered
    // to point to the same place.
    TEST( SortedDataInterface, CursorsPointToSamePlaceReversed ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc2, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), -1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor1->locate( key2, loc2 ) );
            ASSERT( cursor2->locate( key2, loc2 ) );
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );

            cursor1->advance();
            cursor2->advance();
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );

            cursor1->advance();
            cursor2->advance();
            ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that two forward cursors positioned at different keys are not considered
    // to point to the same place.
    TEST( SortedDataInterface, CursorsPointToDifferentKeys ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc2, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), 1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor1->locate( key1, loc1 ) );
            ASSERT( cursor2->locate( key2, loc2 ) );
            ASSERT( !cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( !cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that two reverse cursors positioned at different keys are not considered
    // to point to the same place.
    TEST( SortedDataInterface, CursorsPointToDifferentKeysReversed ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc2, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), -1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor1->locate( key1, loc1 ) );
            ASSERT( cursor2->locate( key2, loc2 ) );
            ASSERT( !cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( !cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that two forward cursors positioned at a duplicate key, but with
    // different DiskLocs are not considered to point to the same place.
    TEST( SortedDataInterface, CursorsPointToDifferentDiskLocs ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc2, true /* allow duplicates */ ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), 1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor1->locate( key1, loc1 ) );
            ASSERT( cursor2->locate( key1, loc2 ) );
            ASSERT( !cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( !cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that two reverse cursors positioned at a duplicate key, but with
    // different DiskLocs are not considered to point to the same place.
    TEST( SortedDataInterface, CursorsPointToDifferentDiskLocsReversed ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc2, true /* allow duplicates */ ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), -1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor1->locate( key1, loc1 ) );
            ASSERT( cursor2->locate( key1, loc2 ) );
            ASSERT( !cursor1->pointsToSamePlaceAs( *cursor2 ) );
            ASSERT( !cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that a forward cursor and a reverse cursor positioned at the same key
    // are considered to point to the same place.
    TEST( SortedDataInterface, CursorPointsToSamePlaceRegardlessOfDirection ) {
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
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc2, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc3, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor1( sorted->newCursor( opCtx.get(), 1 ) );
            scoped_ptr<SortedDataInterface::Cursor> cursor2( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor1->locate( key1, loc1 ) );
            ASSERT( cursor2->locate( key3, loc3 ) );
            // SERVER-15480 the reverse cursor is incorrectly casted to a
            //              cursor of type InMemoryBtreeImpl::ForwardCursor
            // ASSERT( !cursor1->pointsToSamePlaceAs( *cursor2 ) );
            // SERVER-15480 the forward cursor is incorrectly casted to a
            //              cursor of type InMemoryBtreeImpl::ReverseCursor
            // ASSERT( !cursor2->pointsToSamePlaceAs( *cursor1 ) );

            cursor1->advance();
            cursor2->advance();
            // SERVER-15480 the reverse cursor is incorrectly casted to a
            //              cursor of type InMemoryBtreeImpl::ForwardCursor
            // ASSERT( cursor1->pointsToSamePlaceAs( *cursor2 ) );
            // SERVER-15480 the forward cursor is incorrectly casted to a
            //              cursor of type InMemoryBtreeImpl::ReverseCursor
            // ASSERT( cursor2->pointsToSamePlaceAs( *cursor1 ) );

            cursor1->advance();
            cursor2->advance();
            // SERVER-15480 the reverse cursor is incorrectly casted to a
            //              cursor of type InMemoryBtreeImpl::ForwardCursor
            // ASSERT( !cursor1->pointsToSamePlaceAs( *cursor2 ) );
            // SERVER-15480 the forward cursor is incorrectly casted to a
            //              cursor of type InMemoryBtreeImpl::ReverseCursor
            // ASSERT( !cursor2->pointsToSamePlaceAs( *cursor1 ) );
        }
    }

    // Verify that a forward cursor always points to the same place as itself.
    TEST( SortedDataInterface, CursorPointsToSamePlaceAsItself ) {
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
                ASSERT( cursor->pointsToSamePlaceAs( *cursor ) );
                cursor->advance();
            }
            ASSERT( cursor->isEOF() );
        }
    }

    // Verify that a reverse cursor always points to the same place as itself.
    TEST( SortedDataInterface, CursorPointsToSamePlaceAsItselfReversed ) {
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
                ASSERT( cursor->pointsToSamePlaceAs( *cursor ) );
                cursor->advance();
            }
            ASSERT( cursor->isEOF() );
        }
    }

} // namespace mongo
