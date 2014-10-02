// sorted_data_interface_test_cursor_advanceto.cpp

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

    // Insert multiple single-field keys and advance to each of them
    // using a forward cursor by specifying their exact key. When
    // advanceTo() is called on a duplicate key, the cursor is
    // positioned at the next occurrence of that key in ascending
    // order by DiskLoc.
    TEST( SortedDataInterface, AdvanceTo ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc3, true /* allow duplicates */ ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc4, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc5, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 5, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key1, 1, false, keyEnd, keyEndInclusive );
                // SERVER-15489 forward cursor is positioned at first occurrence of key in index
                //              when advanceTo() called on duplicate key
                // ASSERT_EQUALS( key1, cursor->getKey() );
                // ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key2, 1, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key2, cursor->getKey() );
                ASSERT_EQUALS( loc4, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key3, 1, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key3, cursor->getKey() );
                ASSERT_EQUALS( loc5, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key4, 1, false, keyEnd, keyEndInclusive );
                ASSERT( cursor->isEOF() );
            }
        }
    }

    // Insert multiple single-field keys and advance to each of them
    // using a reverse cursor by specifying their exact key. When
    // advanceTo() is called on a duplicate key, the cursor is
    // positioned at the next occurrence of that key in descending
    // order by DiskLoc.
    TEST( SortedDataInterface, AdvanceToReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc4, true /* allow duplicates */ ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc5, true /* allow duplicates */ ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 5, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key3, loc5 ) );
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc5, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key3, 1, false, keyEnd, keyEndInclusive );
                // SERVER-15490 reverse cursor is positioned at last occurrence of key in index
                //              when advanceTo() called on duplicate key
                // ASSERT_EQUALS( key3, cursor->getKey() );
                // ASSERT_EQUALS( loc4, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key2, 1, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key2, cursor->getKey() );
                ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key1, 1, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key0, 1, false, keyEnd, keyEndInclusive );
                ASSERT( cursor->isEOF() );
            }
        }
    }

    // Insert two single-field keys and advance to the larger one using
    // a forward cursor positioned at the smaller one, by specifying a key
    // before the current position of the cursor.
    TEST( SortedDataInterface, AdvanceToKeyBeforeCursorPosition ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key0, 1, false, keyEnd, keyEndInclusive );
                // SERVER-15489 forward cursor is positioned at first key in index
                //              when advanceTo() called with key smaller than any entry
                // ASSERT_EQUALS( key2, cursor->getKey() );
                // ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key0, 1, true, keyEnd, keyEndInclusive );
                // SERVER-15489 forward cursor is positioned at first key in index
                //              when advanceTo() called with key smaller than any entry
                // ASSERT_EQUALS( key2, cursor->getKey() );
                // ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }
        }
    }

    // Insert two single-field keys and advance to the smaller one using
    // a reverse cursor positioned at the larger one, by specifying a key
    // after the current position of the cursor.
    TEST( SortedDataInterface, AdvanceToKeyAfterCursorPositionReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key2, loc2 ) );
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key3, 1, false, keyEnd, keyEndInclusive );
                // SERVER-15490 reverse cursor is positioned at last key in index
                //              when advanceTo() called with key larger than any entry
                // ASSERT_EQUALS( key1, cursor->getKey() );
                // ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key2, loc2 ) );
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key3, 1, true, keyEnd, keyEndInclusive );
                // SERVER-15490 reverse cursor is positioned at last key in index
                //              when advanceTo() called with key larger than any entry
                // ASSERT_EQUALS( key1, cursor->getKey() );
                // ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }
        }
    }

    // Insert a single-field key and advance to EOF using a forward cursor
    // by specifying that exact key. When advanceTo() is called with the key
    // where the cursor is positioned (and it is the last entry for that key),
    // the cursor should advance to EOF regardless of whether it is in non-
    // or inclusive mode.
    TEST( SortedDataInterface, AdvanceToKeyAtCursorPosition ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                // SERVER-15483 forward cursor positioned at last entry in index should move
                //              to EOF when advanceTo() is called with that particular key
                // cursor->advanceTo( key1, 1, false, keyEnd, keyEndInclusive );
                // ASSERT( cursor->isEOF() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key1, 1, true, keyEnd, keyEndInclusive );
                ASSERT( cursor->isEOF() );
            }
        }
    }

    // Insert a single-field key and advance to EOF using a reverse cursor
    // by specifying that exact key. When advanceTo() is called with the key
    // where the cursor is positioned (and it is the first entry for that key),
    // the cursor should advance to EOF regardless of whether it is in non-
    // or inclusive mode.
    TEST( SortedDataInterface, AdvanceToKeyAtCursorPositionReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                // SERVER-15483 reverse cursor positioned at first entry in index should move
                //              to EOF when advanceTo() is called with that particular key
                // cursor->advanceTo( key1, 1, false, keyEnd, keyEndInclusive );
                // ASSERT( cursor->isEOF() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key1, 1, true, keyEnd, keyEndInclusive );
                ASSERT( cursor->isEOF() );
            }
        }
    }

    // Insert multiple single-field keys and advance to each of them using
    // a forward cursor by specifying a key that comes immediately before.
    // When advanceTo() is called in non-inclusive mode, the cursor is
    // positioned at the key that comes after the one specified.
    TEST( SortedDataInterface, AdvanceToExclusive ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc3, true /* allow duplicates */ ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key2, loc4, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc5, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 5, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key1, 1, true, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key2, cursor->getKey() );
                ASSERT_EQUALS( loc4, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key2, 1, true, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key3, cursor->getKey() );
                ASSERT_EQUALS( loc5, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key3, 1, true, keyEnd, keyEndInclusive );
                ASSERT( cursor->isEOF() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                // SERVER-15449 forward cursor positioned at EOF should stay at EOF
                //              when advanceTo() is called
                // cursor->advanceTo( key4, 1, true, keyEnd, keyEndInclusive );
                // ASSERT( cursor->isEOF() );
            }
        }
    }

    // Insert multiple single-field keys and advance to each of them using
    // a reverse cursor by specifying a key that comes immediately after.
    // When advanceTo() is called in non-inclusive mode, the cursor is
    // positioned at the key that comes before the one specified.
    TEST( SortedDataInterface, AdvanceToExclusiveReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

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
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc4, true /* allow duplicates */ ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc5, true /* allow duplicates */ ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 5, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key3, loc5 ) );
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc5, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key3, 1, true, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key2, cursor->getKey() );
                ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key2, 1, true, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                cursor->advanceTo( key1, 1, true, keyEnd, keyEndInclusive );
                ASSERT( cursor->isEOF() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                // SERVER-15449 reverse cursor positioned at EOF should stay at EOF
                //              when advanceTo() is called
                // cursor->advanceTo( key0, 1, true, keyEnd, keyEndInclusive );
                // ASSERT( cursor->isEOF() );
            }
        }
    }

    // Insert multiple, non-consecutive, single-field keys and advance to
    // each of them using a forward cursor by specifying a key between their
    // exact key and the current position of the cursor.
    TEST( SortedDataInterface, AdvanceToIndirect ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        BSONObj unusedKey = key6; // larger than any inserted key

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc2, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key5, loc3, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key2.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = true;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key3, cursor->getKey() );
                ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key4.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = true;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key5, cursor->getKey() );
                ASSERT_EQUALS( loc3, cursor->getDiskLoc() );
            }
        }
    }

    // Insert multiple, non-consecutive, single-field keys and advance to
    // each of them using a reverse cursor by specifying a key between their
    // exact key and the current position of the cursor.
    TEST( SortedDataInterface, AdvanceToIndirectReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        BSONObj unusedKey = key0; // smaller than any inserted key

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc2, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key5, loc3, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key5, loc3 ) );
            ASSERT_EQUALS( key5, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key4.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = true;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key3, cursor->getKey() );
                ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key2.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = true;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }
        }
    }

    // Insert multiple, non-consecutive, single-field keys and advance to
    // each of them using a forward cursor by specifying a key between their
    // exact key and the current position of the cursor. When advanceTo()
    // is called in non-inclusive mode, the cursor is positioned at the key
    // that comes after the one specified.
    TEST( SortedDataInterface, AdvanceToIndirectExclusive ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        BSONObj unusedKey = key6; // larger than any inserted key

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc2, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key5, loc3, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key2.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = false;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key3, cursor->getKey() );
                ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key4.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = false;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key5, cursor->getKey() );
                ASSERT_EQUALS( loc3, cursor->getDiskLoc() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key3.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = false;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key5, cursor->getKey() );
                ASSERT_EQUALS( loc3, cursor->getDiskLoc() );
            }
        }
    }

    // Insert multiple, non-consecutive, single-field keys and advance to
    // each of them using a reverse cursor by specifying a key between their
    // exact key and the current position of the cursor. When advanceTo()
    // is called in non-inclusive mode, the cursor is positioned at the key
    // that comes before the one specified.
    TEST( SortedDataInterface, AdvanceToIndirectExclusiveReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface() );

        BSONObj unusedKey = key0; // smaller than any inserted key

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key1, loc1, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc2, false ) );
                ASSERT_OK( sorted->insert( opCtx.get(), key5, loc3, false ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 3, sorted->numEntries( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key5, loc3 ) );
            ASSERT_EQUALS( key5, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key4.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = false;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key3, cursor->getKey() );
                ASSERT_EQUALS( loc2, cursor->getDiskLoc() );
            }

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key2.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = false;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key5, loc3 ) );
            ASSERT_EQUALS( key5, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            {
                vector<const BSONElement*> keyEnd( 1 );
                vector<bool> keyEndInclusive( 1 );

                const BSONElement end0 = key3.firstElement();
                keyEnd[0] = &end0;
                keyEndInclusive[0] = false;

                cursor->advanceTo( unusedKey, 0, false, keyEnd, keyEndInclusive );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( key1, cursor->getKey() );
                ASSERT_EQUALS( loc1, cursor->getDiskLoc() );
            }
        }
    }

} // namespace mongo
