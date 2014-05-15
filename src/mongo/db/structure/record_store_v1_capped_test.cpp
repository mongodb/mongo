// record_store_v1_capped_test.cpp

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

#include "mongo/db/structure/record_store_v1_capped.h"

#include "mongo/db/storage/record.h"
#include "mongo/db/structure/record_store_v1_test_help.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    class DummyCappedDocumentDeleteCallback : public CappedDocumentDeleteCallback {
    public:
        Status aboutToDeleteCapped( OperationContext* txn, const DiskLoc& loc ) {
            deleted.push_back( loc );
            return Status::OK();
        }
        vector<DiskLoc> deleted;
    };

    void simpleInsertTest( const char* buf, int size ) {

        DummyOperationContext txn;
        DummyExtentManager em;
        DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData( true, 0 );
        DummyCappedDocumentDeleteCallback cb;

        string myns = "test.simple1";
        CappedRecordStoreV1 rs( &txn, &cb, myns, md, &em, false );

        rs.increaseStorageSize( &txn, 1024, -1 );

        ASSERT_NOT_OK( rs.insertRecord( &txn, buf, 3, 1000 ).getStatus() );

        rs.insertRecord( &txn, buf, size, 10000 );

        {
            BSONObjBuilder b;
            int64_t storageSize = rs.storageSize( &b );
            BSONObj obj = b.obj();
            ASSERT_EQUALS( 1, obj["numExtents"].numberInt() );
            ASSERT_EQUALS( storageSize, em.quantizeExtentSize( 1024 ) );
        }

        for ( int i = 0; i < 1000; i++ ) {
            ASSERT_OK( rs.insertRecord( &txn, buf, size, 10000 ).getStatus() );
        }

        long long start = md->numRecords();
        for ( int i = 0; i < 1000; i++ ) {
            ASSERT_OK( rs.insertRecord( &txn, buf, size, 10000 ).getStatus() );
        }
        ASSERT_EQUALS( start, md->numRecords() );
        ASSERT_GREATER_THAN( start, 100 );
        ASSERT_LESS_THAN( start, 1000 );
    }

    TEST(CappedRecordStoreV1, SimpleInsertSize4) {
        simpleInsertTest("abcd", 4);
    }
    TEST(CappedRecordStoreV1, Simpleinserttes8) {
        simpleInsertTest("abcdefgh", 8);
    }

}
