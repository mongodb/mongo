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

#include <boost/filesystem/operations.hpp>

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/rocks/rocks_btree_impl.h"
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
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( 1 ) );
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
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( 1 ) );
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
                scoped_ptr<SortedDataInterface::Cursor> cursor( btree.newCursor( 1 ) );
                ASSERT( cursor->locate( BSON( "a" << 2 ), DiskLoc(0,0) ) );
                ASSERT( !cursor->isEOF()  );
                ASSERT_EQUALS( BSON( "" << 2 ), cursor->getKey() );
                ASSERT_EQUALS( DiskLoc(1,2), cursor->getDiskLoc() );
            }
        }
    }

}
