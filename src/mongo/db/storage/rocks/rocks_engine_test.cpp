// rocks_engine_test.cpp

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

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/rocks/rocks_collection_catalog_entry.h"
#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace mongo {

    class MyOperationContext : public OperationContextNoop {
    public:
        MyOperationContext( RocksEngine* engine )
            : OperationContextNoop( new RocksRecoveryUnit( engine->getDB(), false ) ) {
        }
    };

    TEST( RocksEngineTest, Start1 ) {
        std::string path = "/tmp/mongo-rocks-engine-test";
        boost::filesystem::remove_all( path );
        {
            RocksEngine engine( path );
        }

        {
            RocksEngine engine( path );
        }

    }

    TEST( RocksEngineTest, CreateDirect1 ) {
        std::string path = "/tmp/mongo-rocks-engine-test";
        boost::filesystem::remove_all( path );
        RocksEngine engine( path );

        {
            MyOperationContext opCtx( &engine );
            Status status = engine.createCollection( &opCtx,
                                                     "test.foo",
                                                     CollectionOptions() );
            ASSERT_OK( status );
        }

        RocksRecordStore* rs = engine.getEntry( "test.foo" )->recordStore.get();
        string s = "eliot was here";

        {
            MyOperationContext opCtx( &engine );
            DiskLoc loc;
            {
                WriteUnitOfWork uow( &opCtx );
                StatusWith<DiskLoc> res = rs->insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
            }

            ASSERT_EQUALS( s, rs->dataFor( loc ).data() );
        }
    }

    TEST( RocksEngineTest, DropDirect1 ) {
        std::string path = "/tmp/mongo-rocks-engine-test";
        boost::filesystem::remove_all( path );
        RocksEngine engine( path );

        {
            MyOperationContext opCtx( &engine );
            Status status = engine.createCollection( &opCtx,
                                                     "test.foo",
                                                     CollectionOptions() );
            ASSERT_OK( status );
        }

        {
            MyOperationContext opCtx( &engine );
            Status status = engine.createCollection( &opCtx,
                                                     "test.bar",
                                                     CollectionOptions() );
            ASSERT_OK( status );
        }

        {
            MyOperationContext opCtx( &engine );
            Status status = engine.createCollection( &opCtx,
                                                     "silly.bar",
                                                     CollectionOptions() );
            ASSERT_OK( status );
        }

        {
            std::list<std::string> names;
            engine.getCollectionNamespaces( "test", &names );
            ASSERT_EQUALS( 2U, names.size() );
        }

        {
            std::list<std::string> names;
            engine.getCollectionNamespaces( "silly", &names );
            ASSERT_EQUALS( 1U, names.size() );
        }

        {
            MyOperationContext opCtx( &engine );
            Status status = engine.dropCollection( &opCtx,
                                                   "test.foo" );
            ASSERT_OK( status );
        }

        {
            std::list<std::string> names;
            engine.getCollectionNamespaces( "test", &names );
            ASSERT_EQUALS( 1U, names.size() );
            ASSERT_EQUALS( names.front(), "test.bar" );
        }

        {
            MyOperationContext opCtx( &engine );
            Status status = engine.dropCollection( &opCtx,
                                                   "test.foo" );
            ASSERT_NOT_OK( status );
        }
    }

    TEST( RocksCollectionEntryTest, MetaDataRoundTrip ) {
        RocksCollectionCatalogEntry::MetaData md;
        md.ns = "test.foo";
        md.indexes.push_back( RocksCollectionCatalogEntry::IndexMetaData( BSON( "a" << 1 ),
                                                                          true,
                                                                          DiskLoc( 5, 17 ),
                                                                          false ) );

        BSONObj a = md.toBSON();
        ASSERT_EQUALS( 2, a.nFields() );
        ASSERT_EQUALS( string("test.foo"), a["ns"].String() );
        BSONObj indexes = a["indexes"].Obj();
        ASSERT_EQUALS( 1, indexes.nFields() );
        BSONObj idx = indexes["0"].Obj();
        ASSERT_EQUALS( 5, idx.nFields() );
        ASSERT_EQUALS( BSON( "a" << 1 ), idx["spec"].Obj() );
        ASSERT( idx["ready"].trueValue() );
        ASSERT( !idx["multikey"].trueValue() );
        ASSERT_EQUALS( 5, idx["head_a"].Int() );
        ASSERT_EQUALS( 17, idx["head_b"].Int() );

        RocksCollectionCatalogEntry::MetaData md2;
        md2.parse( a );
        ASSERT_EQUALS( md.indexes[0].head, md2.indexes[0].head );
        ASSERT_EQUALS( a, md2.toBSON() );
    }

    TEST( RocksCollectionEntryTest, IndexCreateAndMod1 ) {
        std::string path = "/tmp/mongo-rocks-engine-test";
        boost::filesystem::remove_all( path );
        RocksEngine engine( path );

        {
            RocksCollectionCatalogEntry coll( &engine, "test.foo" );
            coll.createMetaData();
            ASSERT_EQUALS( 0, coll.getTotalIndexCount() );

            BSONObj spec = BSON( "key" << BSON( "a" << 1 ) <<
                                 "name" << "silly" <<
                                 "ns" << "test.foo" );

            IndexDescriptor desc( NULL, "", spec );

            {
                MyOperationContext opCtx( &engine );
                Status status = coll.prepareForIndexBuild( &opCtx, &desc );
                ASSERT_OK( status );
            }

            ASSERT_EQUALS( 1, coll.getTotalIndexCount() );
            ASSERT_EQUALS( 0, coll.getCompletedIndexCount() );
            ASSERT( !coll.isIndexReady( "silly" ) );

            {
                MyOperationContext opCtx( &engine );
                coll.indexBuildSuccess( &opCtx, "silly" );
            }

            ASSERT_EQUALS( 1, coll.getTotalIndexCount() );
            ASSERT_EQUALS( 1, coll.getCompletedIndexCount() );
            ASSERT( coll.isIndexReady( "silly" ) );

            ASSERT_EQUALS( DiskLoc(), coll.getIndexHead( "silly" ) );
            {
                MyOperationContext opCtx( &engine );
                coll.setIndexHead( &opCtx, "silly", DiskLoc( 123,321 ) );
            }
            ASSERT_EQUALS( DiskLoc(123, 321), coll.getIndexHead( "silly" ) );

            ASSERT( !coll.isIndexMultikey( "silly" ) );
            {
                MyOperationContext opCtx( &engine );
                coll.setIndexIsMultikey( &opCtx, "silly", true );
            }
            ASSERT( coll.isIndexMultikey( "silly" ) );

        }
    }

    TEST( RocksEngineTest, Restart1 ) {
        std::string path = "/tmp/mongo-rocks-engine-test";
        boost::filesystem::remove_all( path );

        string s = "eliot was here";
        DiskLoc loc;

        {
            RocksEngine engine( path );

            {
                MyOperationContext opCtx( &engine );
                WriteUnitOfWork uow( &opCtx );
                Status status = engine.createCollection( &opCtx,
                                                     "test.foo",
                                                     CollectionOptions() );
                ASSERT_OK( status );
                uow.commit();
            }

            RocksRecordStore* rs = engine.getEntry( "test.foo" )->recordStore.get();

            {
                MyOperationContext opCtx( &engine );

                {
                    WriteUnitOfWork uow( &opCtx );
                    StatusWith<DiskLoc> res = rs->insertRecord( &opCtx, s.c_str(), s.size() + 1, -1 );
                    ASSERT_OK( res.getStatus() );
                    loc = res.getValue();
                    uow.commit();
                }

                ASSERT_EQUALS( s, rs->dataFor( loc ).data() );
                engine.cleanShutdown( &opCtx );
            }
        }

        {
            RocksEngine engine( path );
            RocksRecordStore* rs = engine.getEntry( "test.foo" )->recordStore.get();
            ASSERT_EQUALS( s, rs->dataFor( loc ).data() );
        }

    }

}
