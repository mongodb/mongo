// rocks_btree_impl_test.cpp

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

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/rocks/rocks_btree_impl.h"
#include "mongo/db/storage/rocks/rocks_index_entry_comparator.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
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
    static std::unique_ptr<RocksIndexEntryComparator> _rocksComparator(
            new RocksIndexEntryComparator( Ordering::make( BSON( "a" << 1 ) ) ) );

    rocksdb::DB* getDB() {
        string path = "/tmp/mongo-rocks-test";
        boost::filesystem::remove_all( path );

        rocksdb::Options options;
        // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();
        // create the DB if it's not already present
        options.create_if_missing = true;

        // open DB
        rocksdb::DB* db;
        rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
        ASSERT(s.ok());

        return db;
    }

    TEST( RocksRecordStoreTest, BrainDead ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            BSONObj key = BSON( "" << 1 );
            DiskLoc loc( 5, 16 );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    ASSERT( !btree.unindex( &opCtx, key, loc ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    Status res = btree.insert( &opCtx, key, loc, true );
                    ASSERT_OK( res );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    ASSERT( btree.unindex( &opCtx, key, loc ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    btree.unindex( &opCtx, key, loc );
                }
            }

        }
    }

    TEST( RocksRecordStoreTest, Locate1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            BSONObj key = BSON( "" << 1 );
            DiskLoc loc( 5, 16 );

            {

                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );
                ASSERT( !cursor->locate( key, loc ) );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    Status res = btree.insert( &opCtx, key, loc, true );
                    ASSERT_OK( res );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( key, loc ) );
                ASSERT_EQUALS( key, cursor->getKey() );
                ASSERT_EQUALS( loc, cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, Locate2 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
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
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = makeColumnFamily( db.get() );

            RocksBtreeImpl btree( db.get(), cfh.get() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );
                ASSERT_FALSE( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, Snapshots ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );

                // get a cursor
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );

                // insert some more stuff
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
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
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );
                ASSERT( cursor->locate( BSON( "a" << 1 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                // advance to the end
                while ( !cursor->isEOF() ) {
                    cursor->advance();
                }

                ASSERT( cursor->isEOF() );

                // restore position
                cursor->restorePosition();
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

                // advance to the end
                while ( !cursor->isEOF() ) {
                    cursor->advance();
                }

                // restore position
                cursor->restorePosition();
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionAdvanced ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 1 ) );
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

                // go back
                ASSERT( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );

                // restore position, make sure we're at the end
                cursor->restorePosition();
                ASSERT( cursor->isEOF()  );
            }
        }
    }

    TEST( RocksRecordStoreTest, Locate1Reverse ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            BSONObj key = BSON( "" << 1 );
            DiskLoc loc( 5, 16 );

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 0 ) );
                ASSERT( !cursor->locate( key, loc ) );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    Status res = btree.insert( &opCtx, key, loc, true );
                    ASSERT_OK( res );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 0 ) );
                ASSERT( cursor->locate( key, loc ) );
                ASSERT_EQUALS( key, cursor->getKey() );
                ASSERT_EQUALS( loc, cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, LocateInexactReverse ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = makeColumnFamily( db.get() );

            RocksBtreeImpl btree( db.get(), cfh.get() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "a" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "a" << 3 ), DiskLoc(1,1), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 0 ) );
                ASSERT_FALSE( cursor->locate( BSON( "a" << 2 ), DiskLoc(1,1) ) );
                ASSERT_FALSE( cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionReverseSimple ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 2 ), DiskLoc(1,2), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 0 ) );
                ASSERT( cursor->locate( BSON( "a" << 1 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 1 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,1), cursor->getDiskLoc() );

                // save the position
                cursor->savePosition();

                // advance to the end
                while ( !cursor->isEOF() ) {
                    cursor->advance();
                }

                ASSERT( cursor->isEOF() );

                // restore position
                cursor->restorePosition();
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

                // advance to the end
                while ( !cursor->isEOF() ) {
                    cursor->advance();
                }

                // restore position
                cursor->restorePosition();
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

    TEST( RocksRecordStoreTest, SaveAndRestorePositionReverseAdvanced ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksBtreeImpl btree( db.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );

                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 1 ), DiskLoc(1,1), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 3 ), DiskLoc(1,3), true ) );
                    ASSERT_OK( btree.insert( &opCtx, BSON( "" << 4 ), DiskLoc(1,4), true ) );
                }
            }

            {
                MyOperationContext opCtx( db.get() );
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( &opCtx, 0 ) );
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

                // go back
                ASSERT( cursor->locate( BSON( "a" << 3 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 3 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,3), cursor->getDiskLoc() );

                // restore position, make sure we're at the end
                cursor->restorePosition();
                ASSERT( cursor->isEOF() );
            }
        }
    }
}
