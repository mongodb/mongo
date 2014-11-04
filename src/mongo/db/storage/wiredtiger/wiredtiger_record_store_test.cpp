// wiredtiger_record_store_test.cpp

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

#include "mongo/platform/basic.h"

#include <sstream>
#include <string>

#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    using std::string;
    using std::stringstream;

    class WiredTigerHarnessHelper : public HarnessHelper {
    public:
        WiredTigerHarnessHelper() : _dbpath( "wt_test" ), _conn( NULL ) {

            std::stringstream ss;
            ss << "create,";
            ss << "statistics=(all),";
            string config = ss.str();
            int ret = wiredtiger_open( _dbpath.path().c_str(), NULL, config.c_str(), &_conn);
            invariantWTOK( ret );

            _sessionCache = new WiredTigerSessionCache( _conn );
        }

        ~WiredTigerHarnessHelper() {
            delete _sessionCache;
            _conn->close(_conn, NULL);
        }

        virtual RecordStore* newNonCappedRecordStore() { return newNonCappedRecordStore("a.b"); }
        RecordStore* newNonCappedRecordStore(const std::string& ns) {
            WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit( _sessionCache );
            OperationContextNoop txn( ru );
            string uri = "table:" + ns;

            StatusWith<std::string> result =
                WiredTigerRecordStore::generateCreateString(ns, CollectionOptions(), "");
            ASSERT_TRUE(result.isOK());
            std::string config = result.getValue();

            {
                WriteUnitOfWork uow(&txn);
                WT_SESSION* s = ru->getSession()->getSession();
                invariantWTOK( s->create( s, uri.c_str(), config.c_str() ) );
                uow.commit();
            }

            return new WiredTigerRecordStore( &txn, ns, uri );
        }

        virtual RecordStore* newCappedRecordStore( int64_t cappedMaxSize,
                                                   int64_t cappedMaxDocs ) {
            std::string ns = "a.b";

            WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit( _sessionCache );
            OperationContextNoop txn( ru );
            string uri = "table:a.b";

            StatusWith<std::string> result =
                WiredTigerRecordStore::generateCreateString("", CollectionOptions(), "");
            ASSERT_TRUE(result.isOK());
            std::string config = result.getValue();

            {
                WriteUnitOfWork uow(&txn);
                WT_SESSION* s = ru->getSession()->getSession();
                invariantWTOK( s->create( s, uri.c_str(), config.c_str() ) );
                uow.commit();
            }

            return new WiredTigerRecordStore( &txn, ns, uri, true, cappedMaxSize, cappedMaxDocs );
        }

        virtual RecoveryUnit* newRecoveryUnit() {
            return new WiredTigerRecoveryUnit( _sessionCache );
        }
    private:
        unittest::TempDir _dbpath;
        WT_CONNECTION* _conn;
        WiredTigerSessionCache* _sessionCache;
    };

    HarnessHelper* newHarnessHelper() {
        return new WiredTigerHarnessHelper();
    }

    TEST(WiredTigerRecordStoreTest, GenerateCreateStringUnknownField) {
        CollectionOptions options;
        options.storageEngine = fromjson("{wiredtiger: {unknownField: 1}}");
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString("", options, "");
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, status.code());
    }

    TEST(WiredTigerRecordStoreTest, GenerateCreateStringNonStringConfig) {
        CollectionOptions options;
        options.storageEngine = fromjson("{wiredtiger: {configString: 12345}}");
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString("", options, "");
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status.code());
    }

    TEST(WiredTigerRecordStoreTest, Isolation1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        DiskLoc loc1;
        DiskLoc loc2;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();

                res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc2 = res.getValue();

                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> t1( harnessHelper->newOperationContext() );
            scoped_ptr<OperationContext> t2( harnessHelper->newOperationContext() );

            scoped_ptr<WriteUnitOfWork> w1( new WriteUnitOfWork( t1.get() ) );
            scoped_ptr<WriteUnitOfWork> w2( new WriteUnitOfWork( t2.get() ) );

            rs->dataFor( t1.get(), loc1 );
            rs->dataFor( t2.get(), loc1 );

            ASSERT_OK( rs->updateRecord( t1.get(), loc1, "b", 2, false, NULL ).getStatus() );
            ASSERT_OK( rs->updateRecord( t1.get(), loc2, "B", 2, false, NULL ).getStatus() );

            try {
                // this should fail
                rs->updateRecord( t2.get(), loc1, "c", 2, false, NULL );
                ASSERT( 0 );
            }
            catch ( WriteConflictException& dle ) {
                w2.reset( NULL );
                t2.reset( NULL );
            }

            w1->commit(); // this should succeed
        }
    }

    TEST(WiredTigerRecordStoreTest, Isolation2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        DiskLoc loc1;
        DiskLoc loc2;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();

                res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc2 = res.getValue();

                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> t1( harnessHelper->newOperationContext() );
            scoped_ptr<OperationContext> t2( harnessHelper->newOperationContext() );

            // ensure we start transactions
            rs->dataFor( t1.get(), loc2 );
            rs->dataFor( t2.get(), loc2 );

            {
                WriteUnitOfWork w( t1.get() );
                ASSERT_OK( rs->updateRecord( t1.get(), loc1, "b", 2, false, NULL ).getStatus() );
                w.commit();
            }

            {
                WriteUnitOfWork w( t2.get() );
                ASSERT_EQUALS( string("a"), rs->dataFor( t2.get(), loc1 ).data() );
                try {
                    // this should fail as our version of loc1 is too old
                    rs->updateRecord( t2.get(), loc1, "c", 2, false, NULL );
                    ASSERT( 0 );
                }
                catch ( WriteConflictException& dle ) {
                }

            }

        }
    }

    TEST(WiredTigerRecordStoreTest, SizeStorer1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        string uri = dynamic_cast<WiredTigerRecordStore*>( rs.get() )->GetURI();

        WiredTigerSizeStorer ss;
        dynamic_cast<WiredTigerRecordStore*>( rs.get() )->setSizeStorer( &ss );

        int N = 12;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                for ( int i = 0; i < N; i++ ) {
                    StatusWith<DiskLoc> res = rs->insertRecord( opCtx.get(), "a", 2, false );
                    ASSERT_OK( res.getStatus() );
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( N, rs->numRecords( opCtx.get() ) );
        }

        rs.reset( NULL );

        {
            long long numRecords;
            long long dataSize;
            ss.load( uri, &numRecords, &dataSize );
            ASSERT_EQUALS( N, numRecords );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            rs.reset( new WiredTigerRecordStore( opCtx.get(), "a.b", uri,
                                                 false, -1, -1, NULL, &ss ) );
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            ASSERT_EQUALS( N, rs->numRecords( opCtx.get() ) );
        }

        string indexUri = "table:myindex";
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            WiredTigerRecoveryUnit* ru =
                dynamic_cast<WiredTigerRecoveryUnit*>( opCtx->recoveryUnit() );

            {
                WriteUnitOfWork uow( opCtx.get() );
                WT_SESSION* s = ru->getSession()->getSession();
                invariantWTOK( s->create( s, indexUri.c_str(), "" ) );
                uow.commit();
            }

            {
                WriteUnitOfWork uow( opCtx.get() );
                ss.storeInto( WiredTigerRecoveryUnit::get( opCtx.get() )->getSession(), indexUri );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            WiredTigerSizeStorer ss2;
            ss2.loadFrom( WiredTigerRecoveryUnit::get( opCtx.get() )->getSession(), indexUri );
            long long numRecords;
            long long dataSize;
            ss2.load( uri, &numRecords, &dataSize );
            ASSERT_EQUALS( N, numRecords );
        }

        rs.reset( NULL ); // this has to be deleted before ss
    }

    StatusWith<DiskLoc> insertBSON(ptr<OperationContext> opCtx, ptr<RecordStore> rs,
                                   const BSONObj& obj) {
        WriteUnitOfWork wuow(opCtx);
        StatusWith<DiskLoc> status = rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), false);
        if (status.isOK())
            wuow.commit();
        return status;
    }

    // TODO make generic
    TEST(WiredTigerRecordStoreTest, OplogHack) {
        WiredTigerHarnessHelper harnessHelper;
        scoped_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.oplog.foo"));
        scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

        // always illegal
        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(2,-1))).getStatus(),
                  ErrorCodes::BadValue);

        ASSERT_EQ(insertBSON(opCtx, rs, BSON("not_ts" << OpTime(2,1))).getStatus(),
                  ErrorCodes::BadValue);

        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << "not an OpTime")).getStatus(),
                  ErrorCodes::BadValue);

        // currently dasserts
        // ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(-2,1))).getStatus(),
                  // ErrorCodes::BadValue);

        // success cases
        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(1,1))).getValue(),
                                                    DiskLoc(1,1));

        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(1,2))).getValue(),
                                                    DiskLoc(1,2));

        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(2,2))).getValue(),
                                                    DiskLoc(2,2));

        // fails because <= highest
        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(2,1))).getStatus(),
                  ErrorCodes::BadValue);

        ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(2,2))).getStatus(),
                  ErrorCodes::BadValue);


        // find start
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(0,1)), DiskLoc()); // nothing <=
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,1)), DiskLoc(1,2)); // between
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,2)), DiskLoc(2,2)); // ==
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,3)), DiskLoc(2,2)); // > highest

        rs->temp_cappedTruncateAfter(opCtx.get(), DiskLoc(2,2),  false); // no-op
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,3)), DiskLoc(2,2));

        rs->temp_cappedTruncateAfter(opCtx.get(), DiskLoc(1,2),  false); // deletes 2,2
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,3)), DiskLoc(1,2));

        rs->temp_cappedTruncateAfter(opCtx.get(), DiskLoc(1,2),  true); // deletes 1,2
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,3)), DiskLoc(1,1));

        {
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(rs->truncate(opCtx.get())); // deletes 1,1 and leaves collection empty
            wuow.commit();
        }
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(2,3)), DiskLoc());
    }

    TEST(WiredTigerRecordStoreTest, OplogHackOnNonOplog) {
        WiredTigerHarnessHelper harnessHelper;
        scoped_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.NOT_oplog.foo"));
        scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

        ASSERT_OK(insertBSON(opCtx, rs, BSON("ts" << OpTime(2,-1))).getStatus());
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), DiskLoc(0,1)), DiskLoc().setInvalid());
    }
}
