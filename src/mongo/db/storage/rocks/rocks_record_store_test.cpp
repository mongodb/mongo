// rocks_record_store_test.cpp

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

#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_noop.h"
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

    TEST( RocksRecoveryUnitTest, Simple1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        db->Put( rocksdb::WriteOptions(), "a", "b" );

        string value;
        db->Get( rocksdb::ReadOptions(), "a", &value );
        ASSERT_EQUALS( value, "b" );

        {
            RocksRecoveryUnit ru( db.get(), false );
            ru.beginUnitOfWork();
            ru.writeBatch()->Put( "a", "c" );

            value = "x";
            db->Get( rocksdb::ReadOptions(), "a", &value );
            ASSERT_EQUALS( value, "b" );

            ru.endUnitOfWork();
            value = "x";
            db->Get( rocksdb::ReadOptions(), "a", &value );
            ASSERT_EQUALS( value, "c" );
        }

    }

    TEST( RocksRecoveryUnitTest, SimpleAbort1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        db->Put( rocksdb::WriteOptions(), "a", "b" );

        {
            string value;
            db->Get( rocksdb::ReadOptions(), "a", &value );
            ASSERT_EQUALS( value, "b" );
        }

        {
            RocksRecoveryUnit ru( db.get(), false );
            ru.beginUnitOfWork();
            ru.writeBatch()->Put( "a", "c" );

            // note: no endUnitOfWork or commitUnitOfWork
        }

        {
            string value;
            db->Get( rocksdb::ReadOptions(), "a", &value );
            ASSERT_EQUALS( value, "b" );
        }
    }


    TEST( RocksRecordStoreTest, Insert1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), db->DefaultColumnFamily() );
            string s = "eliot was here";

            MyOperationContext opCtx( db.get() );
            DiskLoc loc;
            {
                WriteUnitOfWork uow( opCtx.recoveryUnit() );
                StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
            }

            ASSERT_EQUALS( s, rs.dataFor( loc ).data() );
        }
    }

    TEST( RocksRecordStoreTest, Delete1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), db->DefaultColumnFamily() );
            string s = "eliot was here";

            DiskLoc loc;
            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( s, rs.dataFor( loc ).data() );
            }

            ASSERT( rs.dataFor( loc ).data() != NULL );

            {
                MyOperationContext opCtx( db.get() );
                WriteUnitOfWork uow( opCtx.recoveryUnit() );
                rs.deleteRecord( &opCtx, loc );
            }

            ASSERT( rs.dataFor( loc ).data() == NULL );

        }
    }

    TEST( RocksRecordStoreTest, Update1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), db->DefaultColumnFamily() );
            string s1 = "eliot1";
            string s2 = "eliot2 and more";

            DiskLoc loc;
            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx,
                                                               s1.c_str(),
                                                               s1.size() + 1,
                                                               -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( s1, rs.dataFor( loc ).data() );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    StatusWith<DiskLoc> res = rs.updateRecord( &opCtx,
                                                               loc,
                                                               s2.c_str(),
                                                               s2.size() + 1,
                                                               -1,
                                                               NULL );
                    ASSERT_OK( res.getStatus() );
                    ASSERT( loc == res.getValue() );
                }

                ASSERT_EQUALS( s2, rs.dataFor( loc ).data() );
            }

        }
    }

    TEST( RocksRecordStoreTest, UpdateInPlace1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), db->DefaultColumnFamily() );
            string s1 = "aaa111bbb";
            string s2 = "aaa222bbb";

            DiskLoc loc;
            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx,
                                                               s1.c_str(),
                                                               s1.size() + 1,
                                                               -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( s1, rs.dataFor( loc ).data() );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( opCtx.recoveryUnit() );
                    const char* damageSource = "222";
                    mutablebson::DamageVector dv;
                    dv.push_back( mutablebson::DamageEvent() );
                    dv[0].sourceOffset = 0;
                    dv[0].targetOffset = 3;
                    dv[0].size = 3;
                    Status res = rs.updateWithDamages( &opCtx,
                                                       loc,
                                                       damageSource,
                                                       dv );
                    ASSERT_OK( res );
                }
                ASSERT_EQUALS( s2, rs.dataFor( loc ).data() );
            }

        }
    }

    TEST( RocksRecordStoreTest, TwoCollections ) {
        rocksdb::DB* db = getDB();

        rocksdb::ColumnFamilyHandle* cf1;
        rocksdb::ColumnFamilyHandle* cf2;

        rocksdb::Status status;

        status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar1", &cf1 );
        ASSERT( status.ok() );
        status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar2", &cf2 );
        ASSERT( status.ok() );

        RocksRecordStore rs1( "foo.bar1", db, cf1 );
        RocksRecordStore rs2( "foo.bar2", db, cf2 );

        DiskLoc a;
        DiskLoc b;

        {
            MyOperationContext opCtx( db );
            WriteUnitOfWork uow( opCtx.recoveryUnit() );

            StatusWith<DiskLoc> result = rs1.insertRecord( &opCtx, "a", 2, -1 );
            ASSERT_OK( result.getStatus() );
            a = result.getValue();

            result = rs2.insertRecord( &opCtx, "b", 2, -1 );
            ASSERT_OK( result.getStatus() );
            b = result.getValue();
        }

        ASSERT_EQUALS( a, b );

        ASSERT_EQUALS( string("a"), rs1.dataFor( a ).data() );
        ASSERT_EQUALS( string("b"), rs2.dataFor( b ).data() );

        delete cf2;
        delete cf1;

        delete db;
    }

    TEST( RocksRecordStoreTest, Stats1 ) {
        scoped_ptr<rocksdb::DB> db( getDB() );

        RocksRecordStore rs( "foo.bar", db.get(), db->DefaultColumnFamily() );
        string s = "eliot was here";

        {
            MyOperationContext opCtx( db.get() );
            DiskLoc loc;
            {
                WriteUnitOfWork uow( opCtx.recoveryUnit() );
                StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
            }

            ASSERT_EQUALS( s, rs.dataFor( loc ).data() );
        }

        {
            MyOperationContext opCtx( db.get() );
            BSONObjBuilder b;
            rs.appendCustomStats( &opCtx, &b, 1 );
            BSONObj obj = b.obj();
            ASSERT( obj["stats"].String().find( "WAL" ) != string::npos );
        }
    }

}
