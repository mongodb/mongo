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

#include <memory>

#include <boost/filesystem/operations.hpp>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace mongo {

    class MyOperationContext : public OperationContextNoop {
    public:
        MyOperationContext( rocksdb::DB* db )
            : OperationContextNoop( new RocksRecoveryUnit( db, false ) ) { }
    };

    // to be used in testing
    static boost::scoped_ptr<rocksdb::Comparator> _rocksComparator(
            RocksRecordStore::newRocksCollectionComparator() );

    rocksdb::ColumnFamilyOptions getColumnFamilyOptions() {
        rocksdb::ColumnFamilyOptions options;
        options.comparator = _rocksComparator.get();
        return options;
    }

    // the name of the column family that will be used to back the data in all the record stores
    // created during tests.
    const string columnFamilyName = "myColumnFamily";

    boost::shared_ptr<rocksdb::ColumnFamilyHandle> _createCfh(rocksdb::DB* db ) {

        rocksdb::ColumnFamilyHandle* cfh;

        rocksdb::Status s = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(),
                                                    columnFamilyName,
                                                    &cfh );

        invariant( s.ok() );

        return boost::shared_ptr<rocksdb::ColumnFamilyHandle>( cfh );
    }

    string _rocksRecordStoreTestDir = "mongo-rocks-test";

    rocksdb::DB* getDB( string path) {
        boost::filesystem::remove_all( path );

        rocksdb::Options options = RocksEngine::dbOptions();

        // open DB
        rocksdb::DB* db;
        rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
        ASSERT_OK( toMongoStatus( s ) );

        return db;
    }

    typedef std::pair<shared_ptr<rocksdb::DB>, shared_ptr<rocksdb::ColumnFamilyHandle> > DbAndCfh;
    DbAndCfh getDBPersist( string path ) {
        // Need to pass a vector with cfd's for every column family, which should just be
        // columnFamilyName (for data) and the rocks default column family (for metadata).
        vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        descriptors.push_back( rocksdb::ColumnFamilyDescriptor() );
        descriptors.push_back( rocksdb::ColumnFamilyDescriptor( columnFamilyName,
                                                                rocksdb::ColumnFamilyOptions() ) );

        // open DB
        rocksdb::DB* db;
        rocksdb::Options options = RocksEngine::dbOptions();
        vector<rocksdb::ColumnFamilyHandle*> handles;
        rocksdb::Status s = rocksdb::DB::Open(options, path, descriptors, &handles, &db);
        ASSERT_OK( toMongoStatus( s ) );

        // so that the caller of this function has access to the column family handle backing the
        // record store data.
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfhPtr( handles[1] );

        return std::make_pair( boost::shared_ptr<rocksdb::DB>( db ), cfhPtr );
    }

    TEST( RocksRecoveryUnitTest, Simple1 ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

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
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

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
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = _createCfh( db.get() );
        int size;

        {
            RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );
            string s = "eliot was here";
            size = s.length() + 1;

            MyOperationContext opCtx( db.get() );
            DiskLoc loc;
            {
                WriteUnitOfWork uow( &opCtx );
                StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
            }

            ASSERT_EQUALS( s, rs.dataFor( NULL,  loc ).data() );
        }

        {
            RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );
            ASSERT_EQUALS( 1, rs.numRecords( NULL ) );
            ASSERT_EQUALS( size, rs.dataSize( NULL ) );
        }
    }

    TEST( RocksRecordStoreTest, Delete1 ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = _createCfh( db.get() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );
            string s = "eliot was here";

            DiskLoc loc;
            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.insertRecord(&opCtx,
                                                              s.c_str(),
                                                              s.size() + 1,
                                                              -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( s, rs.dataFor( NULL,  loc ).data() );
                ASSERT_EQUALS( 1, rs.numRecords( NULL ) );
                ASSERT_EQUALS( static_cast<long long> ( s.length() + 1 ), rs.dataSize( NULL ) );
            }

            ASSERT( rs.dataFor( NULL,  loc ).data() != NULL );

            {
                MyOperationContext opCtx( db.get() );
                WriteUnitOfWork uow( &opCtx );
                rs.deleteRecord( &opCtx, loc );

                ASSERT_EQUALS( 0, rs.numRecords( NULL ) );
                ASSERT_EQUALS( 0, rs.dataSize( NULL ) );
            }
        }
    }

    TEST( RocksRecordStoreTest, Update1 ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = _createCfh( db.get() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );
            string s1 = "eliot1";
            string s2 = "eliot2 and more";

            DiskLoc loc;
            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx,
                                                               s1.c_str(),
                                                               s1.size() + 1,
                                                               -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( s1, rs.dataFor( NULL,  loc ).data() );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.updateRecord( &opCtx,
                                                               loc,
                                                               s2.c_str(),
                                                               s2.size() + 1,
                                                               -1,
                                                               NULL );
                    ASSERT_OK( res.getStatus() );
                    ASSERT( loc == res.getValue() );
                }

                ASSERT_EQUALS( s2, rs.dataFor( NULL,  loc ).data() );
            }

        }
    }

    TEST( RocksRecordStoreTest, UpdateInPlace1 ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = _createCfh( db.get() );

        {
            RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );
            string s1 = "aaa111bbb";
            string s2 = "aaa222bbb";

            DiskLoc loc;
            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx,
                                                               s1.c_str(),
                                                               s1.size() + 1,
                                                               -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( s1, rs.dataFor( NULL,  loc ).data() );
            }

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
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
                ASSERT_EQUALS( s2, rs.dataFor( NULL,  loc ).data() );
            }

        }
    }

    TEST( RocksRecordStoreTest, TwoCollections ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        rocksdb::ColumnFamilyHandle* cf1;
        rocksdb::ColumnFamilyHandle* cf2;
        rocksdb::ColumnFamilyHandle* cf1_m;
        rocksdb::ColumnFamilyHandle* cf2_m;

        rocksdb::Status status;

        status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar1", &cf1 );
        ASSERT_OK( toMongoStatus( status ) );
        status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar2", &cf2 );
        ASSERT_OK( toMongoStatus( status ) );

        status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar1&", &cf1_m );
        ASSERT_OK( toMongoStatus( status ) );
        status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar2&", &cf2_m );
        ASSERT_OK( toMongoStatus( status ) );

        RocksRecordStore rs1( "foo.bar1", db.get(), cf1, cf1_m );
        RocksRecordStore rs2( "foo.bar2", db.get(), cf2, cf2_m );

        DiskLoc a;
        DiskLoc b;

        {
            MyOperationContext opCtx( db.get() );
            WriteUnitOfWork uow( &opCtx );

            StatusWith<DiskLoc> result = rs1.insertRecord( &opCtx, "a", 2, -1 );
            ASSERT_OK( result.getStatus() );
            a = result.getValue();

            result = rs2.insertRecord( &opCtx, "b", 2, -1 );
            ASSERT_OK( result.getStatus() );
            b = result.getValue();
        }

        ASSERT_EQUALS( a, b );

        ASSERT_EQUALS( string("a"), rs1.dataFor( NULL,  a ).data() );
        ASSERT_EQUALS( string("b"), rs2.dataFor( NULL,  b ).data() );

        delete cf2;
        delete cf1;
    }

    TEST( RocksRecordStoreTest, Stats1 ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );
        boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = _createCfh( db.get() );

        RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );
        string s = "eliot was here";

        {
            MyOperationContext opCtx( db.get() );
            DiskLoc loc;
            {
                WriteUnitOfWork uow( &opCtx );
                StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
            }

            ASSERT_EQUALS( s, rs.dataFor( NULL,  loc ).data() );
        }

        {
            MyOperationContext opCtx( db.get() );
            BSONObjBuilder b;
            rs.appendCustomStats( &opCtx, &b, 1 );
            BSONObj obj = b.obj();
            ASSERT( obj["stats"].String().find( "WAL" ) != string::npos );
        }
    }

    TEST( RocksRecordStoreTest, Persistence1 ) {
        DiskLoc loc;
        string origStr = "eliot was here";
        string newStr = "antonio was here";
        unittest::TempDir td( _rocksRecordStoreTestDir );

        {
            scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> cfh = _createCfh( db.get() );

            RocksRecordStore rs( "foo.bar", db.get(), cfh.get(), db->DefaultColumnFamily() );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, origStr.c_str(),
                                                               origStr.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                }

                ASSERT_EQUALS( origStr, rs.dataFor( NULL,  loc ).data() );
            }
        }

        {
            DbAndCfh dbAndCfh = getDBPersist( td.path() );
            boost::shared_ptr<rocksdb::DB> db = dbAndCfh.first;

            RocksRecordStore rs( "foo.bar",
                                 db.get(),
                                 dbAndCfh.second.get(),
                                 db->DefaultColumnFamily() );

            ASSERT_EQUALS( static_cast<long long> ( origStr.size() + 1 ), rs.dataSize( NULL ) );
            ASSERT_EQUALS( 1, rs.numRecords( NULL ) );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.updateRecord( &opCtx, loc, newStr.c_str(),
                                                               newStr.size() + 1, -1, NULL );
                    ASSERT_OK( res.getStatus() );
                }

                ASSERT_EQUALS( newStr, rs.dataFor( NULL,  loc ).data() );
            }
        }

        {
            DbAndCfh dbAndCfh = getDBPersist( td.path() );
            boost::shared_ptr<rocksdb::DB> db = dbAndCfh.first;

            RocksRecordStore rs( "foo.bar",
                                 db.get(),
                                 dbAndCfh.second.get(),
                                 db->DefaultColumnFamily() );

            ASSERT_EQUALS( static_cast<long long>( newStr.size() + 1 ), rs.dataSize( NULL ) );
            ASSERT_EQUALS( 1, rs.numRecords( NULL ) );

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    rs.deleteRecord( &opCtx, loc );
                }
            }

            ASSERT_EQUALS( 0, rs.dataSize( NULL ) );
            ASSERT_EQUALS( 0, rs.numRecords( NULL ) );
        }
    }

    TEST( RocksRecordStoreTest, ForwardIterator ) {
        {
            unittest::TempDir td( _rocksRecordStoreTestDir );
            scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

            rocksdb::ColumnFamilyHandle* cf1;
            rocksdb::ColumnFamilyHandle* cf1_m;

            rocksdb::Status status;

            status = db->CreateColumnFamily( getColumnFamilyOptions(), "foo.bar1", &cf1 );
            ASSERT_OK( toMongoStatus( status ) );
            status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar1&", &cf1_m );
            ASSERT_OK( toMongoStatus( status ) );

            RocksRecordStore rs( "foo.bar", db.get(), cf1, cf1_m );
            string s1 = "eliot was here";
            string s2 = "antonio was here";
            string s3 = "eliot and antonio were here";
            DiskLoc loc1;
            DiskLoc loc2;
            DiskLoc loc3;

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s1.c_str(), s1.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc1 = res.getValue();
                    res = rs.insertRecord( &opCtx, s2.c_str(), s2.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc2 = res.getValue();
                    res = rs.insertRecord( &opCtx, s3.c_str(), s3.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc3 = res.getValue();
                }
            }

            OperationContextNoop txn;

            scoped_ptr<RecordIterator> iter( rs.getIterator( &txn ) );

            ASSERT_EQUALS( false, iter->isEOF() );
            ASSERT_EQUALS( loc1, iter->getNext() );
            ASSERT_EQUALS( s1, iter->dataFor( loc1 ).data() );

            ASSERT_EQUALS( false, iter->isEOF() );
            ASSERT_EQUALS( loc2, iter->getNext() );
            ASSERT_EQUALS( s2, iter->dataFor( loc2 ).data() );

            ASSERT_EQUALS( false, iter->isEOF() );
            ASSERT_EQUALS( loc3, iter->getNext() );
            ASSERT_EQUALS( s3, iter->dataFor( loc3 ).data() );

            ASSERT_EQUALS( true, iter->isEOF() );
            ASSERT_EQUALS( DiskLoc(), iter->getNext() );
        }
    }

    TEST( RocksRecordStoreTest, BackwardIterator ) {
        {
            unittest::TempDir td( _rocksRecordStoreTestDir );
            scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

            rocksdb::ColumnFamilyHandle* cf1;
            rocksdb::ColumnFamilyHandle* cf1_m;

            rocksdb::Status status;

            status = db->CreateColumnFamily( getColumnFamilyOptions(), "foo.bar1", &cf1 );
            ASSERT_OK( toMongoStatus( status ) );
            status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar1&", &cf1_m );
            ASSERT_OK( toMongoStatus( status ) );

            RocksRecordStore rs( "foo.bar", db.get(), cf1, cf1_m );
            string s1 = "eliot was here";
            string s2 = "antonio was here";
            string s3 = "eliot and antonio were here";
            DiskLoc loc1;
            DiskLoc loc2;
            DiskLoc loc3;

            {
                MyOperationContext opCtx( db.get() );
                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s1.c_str(), s1.size() +1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc1 = res.getValue();
                    res = rs.insertRecord( &opCtx, s2.c_str(), s2.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc2 = res.getValue();
                    res = rs.insertRecord( &opCtx, s3.c_str(), s3.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc3 = res.getValue();
                }
            }

            OperationContextNoop txn;
            scoped_ptr<RecordIterator> iter( rs.getIterator( &txn, maxDiskLoc, false,
                                             CollectionScanParams::BACKWARD ) );
            ASSERT_EQUALS( false, iter->isEOF() );
            ASSERT_EQUALS( loc3, iter->getNext() );
            ASSERT_EQUALS( s3, iter->dataFor( loc3 ).data() );

            ASSERT_EQUALS( false, iter->isEOF() );
            ASSERT_EQUALS( loc2, iter->getNext() );
            ASSERT_EQUALS( s2, iter->dataFor( loc2 ).data() );

            ASSERT_EQUALS( false, iter->isEOF() );
            ASSERT_EQUALS( loc1, iter->getNext() );
            ASSERT_EQUALS( s1, iter->dataFor( loc1 ).data() );

            ASSERT_EQUALS( true, iter->isEOF() );
            ASSERT_EQUALS( DiskLoc(), iter->getNext() );
        }
    }

    TEST( RocksRecordStoreTest, Truncate1 ) {
        unittest::TempDir td( _rocksRecordStoreTestDir );
        scoped_ptr<rocksdb::DB> db( getDB( td.path() ) );

        {
            rocksdb::ColumnFamilyHandle* cf1;
            rocksdb::ColumnFamilyHandle* cf1_m;

            rocksdb::Status status;

            status = db->CreateColumnFamily( getColumnFamilyOptions(), "foo.bar1", &cf1 );
            ASSERT_OK( toMongoStatus( status ) );
            status = db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), "foo.bar1&", &cf1_m );
            ASSERT_OK( toMongoStatus( status ) );

            RocksRecordStore rs( "foo.bar", db.get(), cf1, cf1_m );
            string s = "antonio was here";

            {
                MyOperationContext opCtx( db.get() );
                WriteUnitOfWork uow( &opCtx );
                StatusWith<DiskLoc> res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
                res = rs.insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
            }

            {
                MyOperationContext opCtx( db.get() );
                WriteUnitOfWork uow( &opCtx );
                Status stat = rs.truncate( &opCtx );
                ASSERT_OK( stat );

                ASSERT_EQUALS( 0, rs.numRecords( NULL ) );
                ASSERT_EQUALS( 0, rs.dataSize( NULL ) );
            }

            // Test that truncate does not fail on an empty collection
            {
                MyOperationContext opCtx( db.get() );
                WriteUnitOfWork uow( &opCtx );
                Status stat = rs.truncate( &opCtx );
                ASSERT_OK( stat );

                ASSERT_EQUALS( 0, rs.numRecords( NULL ) );
                ASSERT_EQUALS( 0, rs.dataSize( NULL ) );
            }
        }
    }
}
