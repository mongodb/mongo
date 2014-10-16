// sorted_data_interface_test_cursor_locate.cpp

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

    // Insert a key and try to locate it using a forward cursor
    // by specifying its exact key and DiskLoc.
    TEST( SortedDataInterface, Locate ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( key1, loc1 ) );
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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert a key and try to locate it using a reverse cursor
    // by specifying its exact key and DiskLoc.
    TEST( SortedDataInterface, LocateReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( key1, loc1 ) );
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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert a compound key and try to locate it using a forward cursor
    // by specifying its exact key and DiskLoc.
    TEST( SortedDataInterface, LocateCompoundKey ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( compoundKey1a, loc1 ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1a, loc1, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( compoundKey1a, loc1 ) );
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert a compound key and try to locate it using a reverse cursor
    // by specifying its exact key and DiskLoc.
    TEST( SortedDataInterface, LocateCompoundKeyReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( compoundKey1a, loc1 ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1a, loc1, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( compoundKey1a, loc1 ) );
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple keys and try to locate them using a forward cursor
    // by specifying their exact key and DiskLoc.
    TEST( SortedDataInterface, LocateMultiple ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( key1, loc1 ) );
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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( key2, loc2 ) );
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );

            ASSERT( cursor->locate( key1, loc1 ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple keys and try to locate them using a reverse cursor
    // by specifying their exact key and DiskLoc.
    TEST( SortedDataInterface, LocateMultipleReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( key3, loc1 ) );
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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key2, loc2 ) );
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( key2, loc2 ) );
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );

            ASSERT( cursor->locate( key3, loc3 ) );
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple compound keys and try to locate them using a forward cursor
    // by specifying their exact key and DiskLoc.
    TEST( SortedDataInterface, LocateMultipleCompoundKeys ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( compoundKey1a, loc1 ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1a, loc1, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1b, loc2, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey2b, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( compoundKey1a, loc1 ) );
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey2b, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1c, loc4, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey3a, loc5, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( cursor->locate( compoundKey1a, loc1 ) );
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1c, cursor->getKey() );
            ASSERT_EQUALS( loc4, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey2b, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey3a, cursor->getKey() );
            ASSERT_EQUALS( loc5, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple compound keys and try to locate them using a reverse cursor
    // by specifying their exact key and DiskLoc.
    TEST( SortedDataInterface, LocateMultipleCompoundKeysReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( compoundKey3a, loc1 ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1a, loc1, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1b, loc2, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey2b, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( compoundKey2b, loc3 ) );
            ASSERT_EQUALS( compoundKey2b, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1c, loc4, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey3a, loc5, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( cursor->locate( compoundKey3a, loc5 ) );
            ASSERT_EQUALS( compoundKey3a, cursor->getKey() );
            ASSERT_EQUALS( loc5, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey2b, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1c, cursor->getKey() );
            ASSERT_EQUALS( loc4, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple keys and try to locate them using a forward cursor
    // by specifying either a smaller key or DiskLoc.
    TEST( SortedDataInterface, LocateIndirect ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( key1, loc1 ) );
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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( !cursor->locate( key1, maxDiskLoc ) );
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( !cursor->locate( key1, minDiskLoc ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple keys and try to locate them using a reverse cursor
    // by specifying either a larger key or DiskLoc.
    TEST( SortedDataInterface, LocateIndirectReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( key3, loc1 ) );
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
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( !cursor->locate( key2, minDiskLoc ) );
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), key3, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( !cursor->locate( key3, maxDiskLoc ) );
            ASSERT_EQUALS( key3, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key2, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( key1, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple compound keys and try to locate them using a forward cursor
    // by specifying either a smaller key or DiskLoc.
    TEST( SortedDataInterface, LocateIndirectCompoundKeys ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );
            ASSERT( !cursor->locate( compoundKey1a, loc1 ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1a, loc1, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1b, loc2, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey2b, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( !cursor->locate( compoundKey1a, maxDiskLoc ) );
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey2b, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1c, loc4, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey3a, loc5, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( !cursor->locate( compoundKey2a, loc1 ) );
            ASSERT_EQUALS( compoundKey2b, cursor->getKey() );
            ASSERT_EQUALS( loc3, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey3a, cursor->getKey() );
            ASSERT_EQUALS( loc5, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Insert multiple compound keys and try to locate them using a reverse cursor
    // by specifying either a larger key or DiskLoc.
    TEST( SortedDataInterface, LocateIndirectCompoundKeysReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );
            ASSERT( !cursor->locate( compoundKey3a, loc1 ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1a, loc1, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1b, loc2, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey2b, loc3, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( !cursor->locate( compoundKey2b, minDiskLoc ) );
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey1c, loc4, true ) );
                ASSERT_OK( sorted->insert( opCtx.get(), compoundKey3a, loc5, true ) );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( !cursor->locate( compoundKey1d, loc1 ) );
            ASSERT_EQUALS( compoundKey1c, cursor->getKey() );
            ASSERT_EQUALS( loc4, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1b, cursor->getKey() );
            ASSERT_EQUALS( loc2, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT_EQUALS( compoundKey1a, cursor->getKey() );
            ASSERT_EQUALS( loc1, cursor->getDiskLoc() );

            cursor->advance();
            ASSERT( cursor->isEOF() );
        }
    }

    // Call locate on a forward cursor of an empty index and verify that the cursor
    // is positioned at EOF.
    TEST( SortedDataInterface, LocateEmpty ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), 1 ) );

            ASSERT( !cursor->locate( BSONObj(), minDiskLoc ) );
            ASSERT( cursor->isEOF() );
        }
    }

    // Call locate on a reverse cursor of an empty index and verify that the cursor
    // is positioned at EOF.
    TEST( SortedDataInterface, LocateEmptyReversed ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<SortedDataInterface> sorted( harnessHelper->newSortedDataInterface( false ) );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT( sorted->isEmpty( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<SortedDataInterface::Cursor> cursor( sorted->newCursor( opCtx.get(), -1 ) );

            ASSERT( !cursor->locate( BSONObj(), maxDiskLoc ) );
            ASSERT( cursor->isEOF() );
        }
    }

} // namespace mongo
