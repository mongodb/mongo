// record_store_test_harness.cpp

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

#include "mongo/db/storage/record_store_test_harness.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    TEST( RecordStoreTestHarness, Simple1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        string s = "eliot was here";

        DiskLoc loc1;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();
                uow.commit();
            }

            ASSERT_EQUALS( s, rs->dataFor( opCtx.get(), loc1 ).data() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( s, rs->dataFor( opCtx.get(), loc1 ).data() );
            ASSERT_EQUALS( 1, rs->numRecords( opCtx.get() ) );

            RecordData rd;
            ASSERT( !rs->findRecord( opCtx.get(), DiskLoc(111,17), &rd ) );
            ASSERT( rd.data() == NULL );

            ASSERT( rs->findRecord( opCtx.get(), loc1, &rd ) );
            ASSERT_EQUALS( s, rd.data() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false );
                ASSERT_OK( res.getStatus() );
                uow.commit();
            }

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 2, rs->numRecords( opCtx.get() ) );
        }
    }

    namespace {
        class DummyDocWriter : public DocWriter {
        public:
            virtual ~DummyDocWriter(){}
            virtual void writeDocument( char* buf ) const {
                memcpy( buf, "eliot", 6 );
            }
            virtual size_t documentSize() const { return 6; }
            virtual bool addPadding() const { return false; }

        };
    }


    TEST( RecordStoreTestHarness, Simple1InsertDocWroter ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );
        
        DiskLoc loc1;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            {
                WriteUnitOfWork uow( opCtx.get() );
                DummyDocWriter dw;
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), &dw, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();
                uow.commit();
            }

            ASSERT_EQUALS( string("eliot"), rs->dataFor( opCtx.get(), loc1 ).data() );
        }
    }

    TEST( RecordStoreTestHarness, Delete1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        string s = "eliot was here";

        DiskLoc loc;
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
                uow.commit();
            }

            ASSERT_EQUALS( s, rs->dataFor( opCtx.get(), loc ).data() );

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, rs->numRecords( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            {
                WriteUnitOfWork uow( opCtx.get() );
                rs->deleteRecord( opCtx.get(), loc );
                uow.commit();
            }

            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

    }

    TEST( RecordStoreTestHarness, Delete2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        string s = "eliot was here";

        DiskLoc loc;
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );

            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false );
                ASSERT_OK( res.getStatus() );
                res = rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
                uow.commit();
            }

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( s, rs->dataFor( opCtx.get(), loc ).data() );
            ASSERT_EQUALS( 2, rs->numRecords( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                rs->deleteRecord( opCtx.get(), loc );
                uow.commit();
            }
        }
    }

    TEST( RecordStoreTestHarness, Update1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        string s1 = "eliot was here";
        string s2 = "eliot was here again";

        DiskLoc loc;
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                           s1.c_str(), s1.size() + 1,
                                                           false );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
                uow.commit();
            }

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( s1, rs->dataFor( opCtx.get(), loc ).data() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->updateRecord( opCtx.get(), loc,
                                                           s2.c_str(), s2.size() + 1,
                                                           false, NULL );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
                uow.commit();
            }

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, rs->numRecords( opCtx.get() ) );
            ASSERT_EQUALS( s2, rs->dataFor( opCtx.get(), loc ).data() );
        }

    }

    TEST( RecordStoreTestHarness, UpdateInPlace1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        string s1 = "aaa111bbb";
        string s2 = "aaa222bbb";

        DiskLoc loc;
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(),
                                                           s1.c_str(),
                                                           s1.size() + 1,
                                                           -1 );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
                uow.commit();
            }

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( s1, rs->dataFor( opCtx.get(), loc ).data() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                const char* damageSource = "222";
                mutablebson::DamageVector dv;
                dv.push_back( mutablebson::DamageEvent() );
                dv[0].sourceOffset = 0;
                dv[0].targetOffset = 3;
                dv[0].size = 3;
                Status res = rs->updateWithDamages( opCtx.get(),
                                                   loc,
                                                   damageSource,
                                                   dv );
                ASSERT_OK( res );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( s2, rs->dataFor( opCtx.get(), loc ).data() );
        }
    }


    TEST( RecordStoreTestHarness, Truncate1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        string s = "eliot was here";

        DiskLoc loc;
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false );
                ASSERT_OK( res.getStatus() );
                loc = res.getValue();
                uow.commit();
            }

        }


        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( s, rs->dataFor( opCtx.get(), loc ).data() );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 1, rs->numRecords( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                rs->truncate( opCtx.get() );
                uow.commit();
            }

        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

    }

    TEST( RecordStoreTestHarness, Cursor1 ) {
        const int N = 10;

        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( 0, rs->numRecords( opCtx.get() ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                for ( int i = 0; i < N; i++ ) {
                    string s = str::stream() << "eliot" << i;
                    ASSERT_OK( rs->insertRecord( opCtx.get(), s.c_str(), s.size() + 1, false ).getStatus() );
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( N, rs->numRecords( opCtx.get() ) );
        }

        {
            int x = 0;
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get() ) );
            while ( !it->isEOF() ) {
                DiskLoc loc = it->getNext();
                RecordData data = it->dataFor( loc );
                string s = str::stream() << "eliot" << x++;
                ASSERT_EQUALS( s, data.data() );
            }
            ASSERT_EQUALS( N, x );
        }

        {
            int x = N;
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(),
                                                           DiskLoc(),
                                                           false,
                                                           CollectionScanParams::BACKWARD ) );
            while ( !it->isEOF() ) {
                DiskLoc loc = it->getNext();
                RecordData data = it->dataFor( loc );
                string s = str::stream() << "eliot" << --x;
                ASSERT_EQUALS( s, data.data() );
            }
            ASSERT_EQUALS( 0, x );
        }

    }

}
