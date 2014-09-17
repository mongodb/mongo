// rocks_sorted_data_impl_test.cpp

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

#include <memory>

#include <boost/shared_ptr.hpp>
#include <boost/filesystem/operations.hpp>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace mongo {

    class MyOperationContext : public OperationContextNoop {
    public:
        MyOperationContext( rocksdb::DB* db )
            : OperationContextNoop( new RocksRecoveryUnit( db, false ) ) {
        }
    };

    // to be used in testing
    static std::unique_ptr<rocksdb::Comparator> _rocksComparator(
            RocksSortedDataImpl::newRocksComparator( Ordering::make( BSON( "a" << 1 ) ) ) );

    string _rocksSortedDataTestDir = "mongo-rocks-test";

    rocksdb::DB* getDB( string path ) {
        boost::filesystem::remove_all( path );

        rocksdb::Options options = RocksEngine::dbOptions();

        // open DB
        rocksdb::DB* db;
        rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
        ASSERT(s.ok());

        return db;
    }

    const Ordering dummyOrdering = Ordering::make( BSONObj() );

    TEST( RocksRecordStoreTest, BrainDead ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            BSONObj key = BSON( "" << 1 );
            DiskLoc loc( 5, 16 );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    ASSERT( !sortedData.unindex( &opCtx, key, loc ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    Status res = sortedData.insert( &opCtx, key, loc, true );
                    ASSERT_OK( res );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    ASSERT( sortedData.unindex( &opCtx, key, loc ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    sortedData.unindex( &opCtx, key, loc );
                    uow.commit();
                }
            }

        }
    }

    TEST( RocksRecordStoreTest, Locate1 ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            BSONObj key = BSON( "" << 1 );
            DiskLoc loc( 5, 16 );

            {

                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( !cursor->locate( key, loc ) );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    Status res = sortedData.insert( &opCtx, key, loc, true );
                    ASSERT_OK( res );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( key, loc ) );
                ASSERT_EQUALS( key, cursor->getKey() );
                ASSERT_EQUALS( loc, cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, Locate2 ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT( cursor->isEOF() );
            }
        }
    }

    boost::shared_ptr<rocksdb::ColumnFamilyHandle> makeColumnFamily( rocksdb::DB* db ) {
        rocksdb::ColumnFamilyOptions options;
        options.comparator = _rocksComparator.get();

        rocksdb::ColumnFamilyHandle* cfh;
        rocksdb::Status s = db->CreateColumnFamily( options, "simpleColumnFamily", &cfh );
        ASSERT( s.ok() );

        return boost::shared_ptr<rocksdb::ColumnFamilyHandle>( cfh );
    }

    TEST( RocksRecordStoreTest, LocateInexact ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = makeColumnFamily( db.get() );

            RocksSortedDataImpl sortedData( db.get(), cfh.get(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT_FALSE( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, Snapshots ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );

                // get a cursor
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );

                // insert some more stuff
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }

                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
                cursor->advance();

                // make sure that the cursor can't "see" anything added after it was created.
                ASSERT( cursor-> isEOF() );
                ASSERT_FALSE( cursor->locate( BSON( "" << 3 ), DiskLoc(1,3) ) );
                ASSERT( cursor->isEOF() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionSimple ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "a" << 1 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // repeat, with a different value
                ASSERT( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionEOF ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "a" << 1 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // advance to the end
                while ( !cursor->isEOF() ) {
                    cursor->advance();
                }

                ASSERT( cursor->isEOF() );

                // save the position
                cursor->savePosition();

                // restore position, make sure we're at the end
                cursor->restorePosition( &opCtx );
                ASSERT( cursor->isEOF()  );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionInsert ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "" << 3 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT_OK(
                                sortedData.insert( &opCtx, BSON( "" << 4 ), DiskLoc(1,4), true ) );
                        uow.commit();
                    }
                }

                // restore position, make sure we don't see the newly inserted value
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT( cursor->isEOF() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionDelete2 ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT( sortedData.unindex( &opCtx, BSON( "" << 1 ), DiskLoc(1,1) ) );
                        uow.commit();
                    }
                }

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionDelete3 ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT( sortedData.unindex( &opCtx, BSON( "" << 3 ), DiskLoc(1,3) ) );
                        uow.commit();
                    }
                }

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // make sure that we can still see the unindexed data, since we're working on
                // a snapshot
                cursor->advance();
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT( cursor->isEOF() );
            }
        }
    }

    TEST( RocksRecordStoreTest, Locate1Reverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            BSONObj key = BSON( "" << 1 );
            DiskLoc loc( 5, 16 );

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, -1 ) );
                ASSERT( !cursor->locate( key, loc ) );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    Status res = sortedData.insert( &opCtx, key, loc, true );
                    ASSERT_OK( res );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, -1 ) );
                ASSERT( cursor->locate( key, loc ) );
                ASSERT_EQUALS( key, cursor->getKey() );
                ASSERT_EQUALS( loc, cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, LocateInexactReverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = makeColumnFamily( db.get() );

            RocksSortedDataImpl sortedData( db.get(), cfh.get(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "a" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "a" << 3 ), DiskLoc(1,1), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, -1 ) );
                ASSERT_FALSE( cursor->locate( BSON( "a" << 2 ), DiskLoc(1,1) ) );
                ASSERT_FALSE( cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionReverseSimple ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, -1 ) );
                ASSERT( cursor->locate( BSON( "a" << 1 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // repeat, with a different value
                ASSERT( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionEOFReverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 4 ), DiskLoc(1,4), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx, -1 ) );
                ASSERT_FALSE( cursor->locate( BSON( "" << 2 ), DiskLoc(1,2) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // advance to the end
                while ( !cursor->isEOF() ) {
                    cursor->advance();
                }

                ASSERT( cursor->isEOF() );

                // save the position
                cursor->savePosition();

                // restore position, make sure we're at the end
                cursor->restorePosition( &opCtx );
                ASSERT( cursor->isEOF() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionInsertReverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx,
                                                                                      -1 ) );
                ASSERT( cursor->locate( BSON( "" << 3 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT_OK(
                                sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                        uow.commit();
                    }
                }

                // restore position, make sure we don't see the newly inserted value
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT( cursor->isEOF() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionDelete1Reverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx,
                                                                                      -1 ) );
                ASSERT( cursor->locate( BSON( "" << 3 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT( sortedData.unindex( &opCtx, BSON( "" << 3 ), DiskLoc(1,3) ) );
                        uow.commit();
                    }
                }

                // restore position, make sure we still see the deleted key and value, because
                // we're using a snapshot
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionDelete2Reverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx,
                                                                                      -1 ) );
                ASSERT( cursor->locate( BSON( "" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT( sortedData.unindex( &opCtx, BSON( "" << 1 ), DiskLoc(1,1) ) );
                        uow.commit();
                    }
                }

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionDelete3Reverse ) {
        unittest::TempDir td( _rocksSortedDataTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            RocksSortedDataImpl sortedData( db.get(), db->DefaultColumnFamily(), dummyOrdering );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );

                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( sortedData.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    uow.commit();
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( sortedData.newCursor( &opCtx,
                                                                                      -1 ) );
                ASSERT( cursor->locate( BSON( "" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                {
                    MyOperationContext opCtx( db.get() );
                    {
                        WriteUnitOfWork uow( &opCtx );
                        ASSERT( sortedData.unindex( &opCtx, BSON( "" << 1 ), DiskLoc(1,1) ) );
                        uow.commit();
                    }
                }

                // restore position
                cursor->restorePosition( &opCtx );
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // make sure that we can still see the unindexed data, since we're working on
                // a snapshot
                cursor->advance();
                ASSERT( !cursor->isEOF() );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                cursor->advance();
                ASSERT( cursor->isEOF() );
            }
        }
    }
}
