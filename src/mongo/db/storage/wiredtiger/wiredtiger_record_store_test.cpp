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

#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
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
        static WT_CONNECTION* createConnection(StringData dbpath, StringData extraStrings) {
            WT_CONNECTION* conn = NULL;

            std::stringstream ss;
            ss << "create,";
            ss << "statistics=(all),";
            ss << extraStrings;
            string config = ss.str();
            int ret = wiredtiger_open(dbpath.toString().c_str(), NULL, config.c_str(), &conn);
            ASSERT_OK(wtRCToStatus(ret));
            ASSERT(conn);

            return conn;
        }

        WiredTigerHarnessHelper()
            : _dbpath("wt_test"),
              _conn(createConnection(_dbpath.path(), "")),
              _sessionCache(new WiredTigerSessionCache(_conn)) { }

        WiredTigerHarnessHelper(StringData extraStrings)
            : _dbpath("wt_test"),
              _conn(createConnection(_dbpath.path(), extraStrings)),
              _sessionCache(new WiredTigerSessionCache(_conn)) { }

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

        virtual RecordStore* newCappedRecordStore( const std::string& ns,
                                                   int64_t cappedMaxSize,
                                                   int64_t cappedMaxDocs ) {

            WiredTigerRecoveryUnit* ru = new WiredTigerRecoveryUnit( _sessionCache );
            OperationContextNoop txn( ru );
            string uri = "table:a.b";

            CollectionOptions options;
            options.capped = true;

            StatusWith<std::string> result =
                WiredTigerRecordStore::generateCreateString(ns, options, "");
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
        options.storageEngine = fromjson("{wiredTiger: {unknownField: 1}}");
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString("", options, "");
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, status.code());
    }

    TEST(WiredTigerRecordStoreTest, GenerateCreateStringNonStringConfig) {
        CollectionOptions options;
        options.storageEngine = fromjson("{wiredTiger: {configString: 12345}}");
        StatusWith<std::string> result = WiredTigerRecordStore::generateCreateString("", options, "");
        const Status& status = result.getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, status.code());
    }

    TEST(WiredTigerRecordStoreTest, Isolation1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        RecordId loc1;
        RecordId loc2;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                StatusWith<RecordId> res = rs->insertRecord( opCtx.get(), "a", 2, false );
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

        RecordId loc1;
        RecordId loc2;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );

                StatusWith<RecordId> res = rs->insertRecord( opCtx.get(), "a", 2, false );
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
                    StatusWith<RecordId> res = rs->insertRecord( opCtx.get(), "a", 2, false );
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

    StatusWith<RecordId> insertBSON(scoped_ptr<OperationContext>& opCtx,
                                   scoped_ptr<RecordStore>& rs,
                                   const OpTime& opTime) {
        BSONObj obj = BSON( "ts" << opTime );
        WriteUnitOfWork wuow(opCtx.get());
        WiredTigerRecordStore* wrs = dynamic_cast<WiredTigerRecordStore*>(rs.get());
        invariant( wrs );
        Status status = wrs->oplogDiskLocRegister( opCtx.get(), opTime );
        if (!status.isOK())
            return StatusWith<RecordId>( status );
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(),
                                                   obj.objdata(),
                                                   obj.objsize(),
                                                   false);
        if (res.isOK())
            wuow.commit();
        return res;
    }

    // TODO make generic
    TEST(WiredTigerRecordStoreTest, OplogHack) {
        WiredTigerHarnessHelper harnessHelper;
        scoped_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.oplog.foo"));
        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

            // always illegal
            ASSERT_EQ(insertBSON(opCtx, rs, OpTime(2,-1)).getStatus(),
                  ErrorCodes::BadValue);

            {
                BSONObj obj = BSON("not_ts" << OpTime(2,1));
                ASSERT_EQ(rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(),
                                           false ).getStatus(),
                          ErrorCodes::BadValue);

                obj = BSON( "ts" << "not an OpTime" );
                ASSERT_EQ(rs->insertRecord(opCtx.get(), obj.objdata(), obj.objsize(),
                                           false ).getStatus(),
                          ErrorCodes::BadValue);
            }

            // currently dasserts
            // ASSERT_EQ(insertBSON(opCtx, rs, BSON("ts" << OpTime(-2,1))).getStatus(),
            // ErrorCodes::BadValue);

            // success cases
            ASSERT_EQ(insertBSON(opCtx, rs, OpTime(1,1)).getValue(),
                      RecordId(1,1));

            ASSERT_EQ(insertBSON(opCtx, rs, OpTime(1,2)).getValue(),
                      RecordId(1,2));

            ASSERT_EQ(insertBSON(opCtx, rs, OpTime(2,2)).getValue(),
                      RecordId(2,2));
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            // find start
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0,1)), RecordId()); // nothing <=
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,1)), RecordId(1,2)); // between
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,2)), RecordId(2,2)); // ==
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,3)), RecordId(2,2)); // > highest
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(2,2),  false); // no-op
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,3)), RecordId(2,2));
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1,2),  false); // deletes 2,2
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,3)), RecordId(1,2));
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            rs->temp_cappedTruncateAfter(opCtx.get(), RecordId(1,2),  true); // deletes 1,2
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,3)), RecordId(1,1));
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(rs->truncate(opCtx.get())); // deletes 1,1 and leaves collection empty
            wuow.commit();
        }

        {
            scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
            ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(2,3)), RecordId());
        }
    }

    TEST(WiredTigerRecordStoreTest, OplogHackOnNonOplog) {
        WiredTigerHarnessHelper harnessHelper;
        scoped_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("local.NOT_oplog.foo"));

        scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());

        BSONObj obj = BSON( "ts" << OpTime(2,-1) );
        {
            WriteUnitOfWork wuow( opCtx.get() );
            ASSERT_OK(rs->insertRecord(opCtx.get(), obj.objdata(),
                                       obj.objsize(), false ).getStatus());
            wuow.commit();
        }
        ASSERT_EQ(rs->oplogStartHack(opCtx.get(), RecordId(0,1)), boost::none);
    }

    TEST(WiredTigerRecordStoreTest, CappedOrder) {
        scoped_ptr<WiredTigerHarnessHelper> harnessHelper( new WiredTigerHarnessHelper() );
        scoped_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("a.b", 100000,10000));

        RecordId loc1;

        { // first insert a document
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                StatusWith<RecordId> res = rs->insertRecord( opCtx.get(), "a", 2, false );
                ASSERT_OK( res.getStatus() );
                loc1 = res.getValue();
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(), loc1 ) );
            ASSERT( !it->isEOF() );
            ASSERT_EQ( loc1, it->getNext() );
            ASSERT( it->isEOF() );
        }

        {
            // now we insert 2 docs, but commit the 2nd one fiirst
            // we make sure we can't find the 2nd until the first is commited
            scoped_ptr<OperationContext> t1( harnessHelper->newOperationContext() );
            scoped_ptr<WriteUnitOfWork> w1( new WriteUnitOfWork( t1.get() ) );
            rs->insertRecord( t1.get(), "b", 2, false );
            // do not commit yet

            { // create 2nd doc
                scoped_ptr<OperationContext> t2( harnessHelper->newOperationContext() );
                {
                    WriteUnitOfWork w2( t2.get() );
                    rs->insertRecord( t2.get(), "c", 2, false );
                    w2.commit();
                }
            }

            { // state should be the same
                scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
                scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(), loc1 ) );
                ASSERT( !it->isEOF() );
                ASSERT_EQ( loc1, it->getNext() );
                ASSERT( it->isEOF() );
            }

            w1->commit();
        }

        { // now all 3 docs should be visible
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(), loc1 ) );
            ASSERT( !it->isEOF() );
            ASSERT_EQ( loc1, it->getNext() );
            ASSERT( !it->isEOF() );
            it->getNext();
            ASSERT( !it->isEOF() );
            it->getNext();
            ASSERT( it->isEOF() );
        }
    }

    RecordId _oplogOrderInsertOplog( OperationContext* txn,
                                    scoped_ptr<RecordStore>& rs,
                                    int inc ) {
        OpTime opTime = OpTime(5,inc);
        WiredTigerRecordStore* wrs = dynamic_cast<WiredTigerRecordStore*>(rs.get());
        Status status = wrs->oplogDiskLocRegister( txn, opTime );
        ASSERT_OK( status );
        BSONObj obj = BSON( "ts" << opTime );
        StatusWith<RecordId> res = rs->insertRecord( txn, obj.objdata(), obj.objsize(), false );
        ASSERT_OK( res.getStatus() );
        return res.getValue();
    }

    TEST(WiredTigerRecordStoreTest, OplogOrder) {
        scoped_ptr<WiredTigerHarnessHelper> harnessHelper( new WiredTigerHarnessHelper() );
        scoped_ptr<RecordStore> rs(harnessHelper->newCappedRecordStore("local.oplog.foo",
                                                                       100000,
                                                                       10000));

        {
            const WiredTigerRecordStore* wrs = dynamic_cast<WiredTigerRecordStore*>(rs.get());
            ASSERT( wrs->isOplog() );
            ASSERT( wrs->usingOplogHack() );
        }

        RecordId loc1;

        { // first insert a document
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                loc1 = _oplogOrderInsertOplog( opCtx.get(), rs, 1 );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(), loc1 ) );
            ASSERT( !it->isEOF() );
            ASSERT_EQ( loc1, it->getNext() );
            ASSERT( it->isEOF() );
        }

        {
            // now we insert 2 docs, but commit the 2nd one fiirst
            // we make sure we can't find the 2nd until the first is commited
            scoped_ptr<OperationContext> t1( harnessHelper->newOperationContext() );
            scoped_ptr<WriteUnitOfWork> w1( new WriteUnitOfWork( t1.get() ) );
            _oplogOrderInsertOplog( t1.get(), rs, 2 );
            // do not commit yet

            { // create 2nd doc
                scoped_ptr<OperationContext> t2( harnessHelper->newOperationContext() );
                {
                    WriteUnitOfWork w2( t2.get() );
                    _oplogOrderInsertOplog( t2.get(), rs, 3 );
                    w2.commit();
                }
            }

            { // state should be the same
                scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
                scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(), loc1 ) );
                ASSERT( !it->isEOF() );
                ASSERT_EQ( loc1, it->getNext() );
                ASSERT( it->isEOF() );
            }

            w1->commit();
        }

        { // now all 3 docs should be visible
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            scoped_ptr<RecordIterator> it( rs->getIterator( opCtx.get(), loc1 ) );
            ASSERT( !it->isEOF() );
            ASSERT_EQ( loc1, it->getNext() );
            ASSERT( !it->isEOF() );
            it->getNext();
            ASSERT( !it->isEOF() );
            it->getNext();
            ASSERT( it->isEOF() );
        }
    }

    TEST(WiredTigerRecordStoreTest, StorageSizeStatisticsDisabled) {
        WiredTigerHarnessHelper harnessHelper("statistics=(none)");
        scoped_ptr<RecordStore> rs(harnessHelper.newNonCappedRecordStore("a.b"));

        scoped_ptr<OperationContext> opCtx(harnessHelper.newOperationContext());
        ASSERT_THROWS(rs->storageSize(opCtx.get()), UserException);
    }

}  // namespace mongo
